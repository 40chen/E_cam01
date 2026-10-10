#!/usr/bin/env python3
"""把 assets/clear-camera-shutter-sound.mp3 转成固件里能直接播的 C 数组。

**一次生成三个候选音色**，因为"这颗喇叭放不放得出快门声"这个问题换过两次前提
（2026-09-19 移除、2026-09-22 换喇叭后恢复），每换一次素材就得重新回答一遍：

    Raw    原素材，只做峰值归一化
    Loud   再过一遍软限幅再归一化：音色不变，RMS 高约 8dB
    Body   再补一层 480Hz 的阻尼正弦，把能量往低中频搬

三个都编进固件，在设置页 SOUND 组的 "Shutter Sound" 一行里点着换、当场试听。
**选定之后应该把另外两个删掉**（每个 26~32KB flash），改下面的 VARIANTS 即可。

这个脚本还会**打印素材的分频段能量占比**。别跳过那段：这个功能前后栽过两次，
两次都是"素材的能量落在喇叭放不出来的频段"，而**光用耳朵在电脑上是听不出来的**
——电脑音箱能放的频段比这颗喇叭宽得多。发报告之前先看一眼那个分布。

为什么源是 mp3、为什么要 ffmpeg：
    素材是 mp3，而固件里没有音频解码器（音乐页那条路是运行时解 MP3，这里是
    编译期就把裸 PCM 编进去），所以必须在电脑上先解好。用 ffmpeg 解成
    **16000Hz / 16bit / 单声道**，后面那套处理是纯 Python 标准库。
    依赖 ffmpeg 这件事和启动页脚本依赖 ImageMagick 是一回事。

为什么目标采样率必须是 16000：
    必须等于 BSP_AUDIO_DEFAULT_RATE（见 lckfb_bsp.h）。等于它的话，
    bsp_audio_set_fs() 从开机到播放全程都是空操作；不等就要在运行期切 I2S 和
    ES8311 的时钟。体积也是原因：415ms 立体声 16bit 在这个采样率下是 26KB。

用法：
    python tools/gen_shutter_sound.py
"""

import math
import os
import subprocess
import sys
import tempfile
from array import array

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SRC = os.path.join(ROOT, "assets", "clear-camera-shutter-sound.mp3")
OUT = os.path.join(ROOT, "main", "app", "shutter_sound.c")

RATE = 16000                # 必须等于 lckfb_bsp.h 的 BSP_AUDIO_DEFAULT_RATE
BYTES_PER_LINE = 16

# 软限幅阈值，相对峰值。越小压得越狠、响度越高，但过头就变成"嗡"不是"咔"。
LIMIT_RATIO = 0.4

# 低中频"身体"：频率贴着喇叭谐振区，衰减留一点尾巴。
BODY_FREQ_HZ = 480.0
BODY_DECAY_MS = 92.0

# (C 里的变量后缀, 设置页显示名, 身体幅度, 是否软限幅, 说明)
#
# **2026-09-22 定稿：只留 Raw。** 换了喇叭又换了素材之后实测"三个候选都有声音"，
# 用户选了 Raw——原素材原样，最像真实快门。另外两个（软限幅提响度、补低中频）
# 是为旧喇叭/旧素材做的补偿，现在都是白占 flash。
#
# 要再加音色就往这个表里加一行，脚本会生成对应的数组和表项，固件那边不用改。
VARIANTS = [
    ("raw",  "Raw",  0.00, False, "原素材，只归一化"),
]

# 打印频谱用的分频段。上面那两档（4k 以上）是重点：如果喇叭在那儿滚降，
# 落在这里的能量就是白扔的
BANDS = [(0, 250), (250, 500), (500, 1000), (1000, 2000),
         (2000, 4000), (4000, 8000)]


def decode_mp3():
    """用 ffmpeg 解成 16000Hz/16bit/单声道 的裸 PCM，返回 float 样本列表。"""
    if not os.path.exists(SRC):
        sys.exit("找不到源文件：%s" % SRC)
    fd, tmp = tempfile.mkstemp(suffix=".pcm")
    os.close(fd)
    try:
        subprocess.run(
            ["ffmpeg", "-v", "error", "-y", "-i", SRC,
             "-ac", "1", "-ar", str(RATE), "-f", "s16le", tmp],
            check=True)
    except FileNotFoundError:
        os.unlink(tmp)
        sys.exit("找不到 ffmpeg 命令，请装 ffmpeg 并加入 PATH")
    except subprocess.CalledProcessError as e:
        os.unlink(tmp)
        sys.exit("ffmpeg 解码失败：%s" % e)

    raw = open(tmp, "rb").read()
    os.unlink(tmp)
    if len(raw) < 2:
        sys.exit("解码出来是空的：%s" % SRC)
    a = array("h")
    a.frombytes(raw)
    if sys.byteorder == "big":              # s16le 是小端
        a.byteswap()
    return [float(v) for v in a]


def report_spectrum(x):
    """打印分频段能量占比。

    自己实现 DFT 而不是用 numpy：这个脚本刻意只依赖标准库（除了 ffmpeg），
    免得"换台机器就跑不起来"。段数不多、信号也短，够用。
    """
    n = 1024
    half = n // 2
    win = [0.5 - 0.5 * math.cos(2.0 * math.pi * i / (n - 1)) for i in range(n)]
    # 提前把每个 bin 的频率算出来，并分好档
    bin_band = []
    for k in range(half + 1):
        f = k * RATE / float(n)
        b = len(BANDS) - 1
        for i, (lo, hi) in enumerate(BANDS):
            if lo <= f < hi:
                b = i
                break
        bin_band.append(b)

    tot = [0.0] * len(BANDS)
    total = 0.0
    # 余弦表：1024 点、每段 512 步，直接现算太慢，先把 cos 表建出来
    cos_tab = [[math.cos(2.0 * math.pi * k * i / n) for i in range(n)]
               for k in range(half + 1)]

    for start in range(0, max(len(x) - n, 0), half):
        seg = x[start:start + n]
        for k in range(half + 1):
            re = 0.0
            ct = cos_tab[k]
            for i in range(n):
                re += seg[i] * win[i] * ct[i]
            p = re * re
            tot[bin_band[k]] += p
            total += p

    print("素材分频段能量占比（%d 点 DFT）：" % n)
    for (lo, hi), v in zip(BANDS, tot):
        pct = 100.0 * v / total if total > 0 else 0.0
        bar = "#" * int(pct / 2.5)
        print("  %5d-%5dHz  %5.1f%%  %s" % (lo, hi, pct, bar))


def add_body(x, freq, decay_ms, level):
    """给高频的"嗒"叠一层低中频的"身体"（指数衰减正弦）。

    机械快门"咔哒"里的那个"哒"本来就是这东西。**只在素材的高频成分喇叭放不出来
    的时候才有意义**——补完之后 1kHz 以下的能量占比能提到 60% 上下，代价是听感
    变"钝"，所以它只是候选之一，不许当成默认值。
    """
    src_peak = max((abs(v) for v in x), default=0.0)
    if src_peak <= 0.0:
        return x

    tail = int(RATE * decay_ms / 1000.0)
    tau = (decay_ms / 1000.0) / 4.6          # 到 decay_ms 时衰减到约 1%
    g = level * src_peak
    n = len(x)

    out = [0.0] * (n + tail)
    for i in range(n + tail):
        t = i / RATE
        out[i] = (x[i] if i < n else 0.0) + \
            math.sin(2.0 * math.pi * freq * t) * math.exp(-t / tau) * g
    return out


def soft_limit(x):
    """tanh 软限幅。

    源素材峰值高、RMS 低（波峰因数接近 19dB）。人耳听的是 RMS 不是峰值，所以
    哪怕归一化到满量程，这个尖脉冲听着还是轻。把峰值用 tanh 压下来再整体抬回去，
    等于把能量往上搬：峰值不变，RMS 涨约 8dB。比直接加大数字音量好——后者是硬
    削波，声音会变"炸"。
    """
    peak = max((abs(v) for v in x), default=0.0)
    if peak <= 0.0:
        return x
    t = peak * LIMIT_RATIO
    return [t * math.tanh(v / t) for v in x]


def to_int16_stereo(x):
    """按峰值归一化，转成交错的 int16：[L0, R0, L1, R1, ...]，单声道复制两份。"""
    peak = max((abs(v) for v in x), default=0.0)
    # 必须能放大，不能只衰减。以前写的是 min(1.0, 32767.0/peak)，那个 clamp 让
    # 归一化实际从未生效过——源峰值 -3.6 dBFS，输出就一直也是 -3.6 dBFS
    g = 32767.0 / peak if peak > 0 else 1.0
    v = array("h")
    for s in x:
        s *= g
        s = 32767.0 if s > 32767 else (-32768.0 if s < -32768 else s)
        i = int(s + 0.5) if s >= 0 else int(s - 0.5)
        v.append(i)                          # 左
        v.append(i)                          # 右
    if sys.byteorder == "big":
        v.byteswap()
    return v


def dbfs(v):
    return 20.0 * math.log10(v / 32768.0) if v > 0 else -999.0


def stats(samples):
    peak = 0
    acc = 0.0
    for v in samples:
        a = abs(v)
        if a > peak:
            peak = a
        acc += float(v) * v
    rms = (acc / len(samples)) ** 0.5 if samples else 0.0
    return dbfs(peak), dbfs(rms)


def emit_array(f, name, samples):
    # 长度必须取字节数：samples 是 array('h')，len() 给的是样本个数，
    # 直接拿它当 uint8_t[] 的维度会小一半，编译器一路报 excess elements
    raw = samples.tobytes()
    f.write("static const uint8_t %s[%d] = {\n" % (name, len(raw)))
    for i in range(0, len(raw), BYTES_PER_LINE):
        f.write("    " + " ".join("0x%02x," % b for b in raw[i:i + BYTES_PER_LINE]) + "\n")
    f.write("};\n\n")


def main():
    src = decode_mp3()
    src_frames = len(src)

    pk = max((abs(v) for v in src), default=1.0)
    rms = (sum(v * v for v in src) / len(src)) ** 0.5
    print("源：%s" % os.path.basename(SRC))
    print("  %d 帧（%.0f ms）  峰值 %.1f dBFS  RMS %.1f dBFS  波峰因数 %.1f dB"
          % (src_frames, src_frames * 1000.0 / RATE,
             dbfs(pk), dbfs(rms), dbfs(pk) - dbfs(rms)))
    report_spectrum(src)
    print()

    built = []
    for suffix, label, body_level, limit, note in VARIANTS:
        x = src
        if body_level > 0:
            x = add_body(x, BODY_FREQ_HZ, BODY_DECAY_MS, body_level)
        if limit:
            x = soft_limit(x)
        v = to_int16_stereo(x)
        peak_db, rms_db = stats(v)
        built.append((suffix, label, v, note, peak_db, rms_db))

    with open(OUT, "w", encoding="utf-8", newline="\n") as f:
        f.write("""/*
 * shutter_sound.c — 快门声 PCM（**自动生成，不要手改**）
 *
 * 由 tools/gen_shutter_sound.py 从 assets/clear-camera-shutter-sound.mp3 生成：
 *   - ffmpeg 解成 16000 Hz / 16bit / 单声道，再复制成两声道交错
 *   - 源素材 %d 帧（%.0f ms），逐版处理见下面每个数组上面的注释
 *   - 采样率**必须**等于 BSP_AUDIO_DEFAULT_RATE，原因见脚本头部注释
 *
 * 三个音色是给"哪个听得见"这个问题准备的。**不要手改这个文件**，
 * 要改音色改脚本里的 VARIANTS 再重新生成：
 *
 *     python tools/gen_shutter_sound.py
 */
#include <stdint.h>

#include "shutter_sound.h"

""" % (src_frames, src_frames * 1000.0 / RATE))

        for suffix, label, v, note, _, _ in built:
            f.write("/* %s — %s */\n" % (label, note))
            emit_array(f, "shutter_pcm_" + suffix, v)

        f.write("const shutter_sound_t shutter_sound_variants[SHUTTER_SOUND_COUNT] = {\n")
        for suffix, label, v, note, peak_db, rms_db in built:
            f.write("    { .name = \"%s\", .pcm = shutter_pcm_%s, .bytes = %d,\n"
                    "      .rate = %d, .frames = %d },\n"
                    % (label, suffix, len(v) * 2, RATE, len(v) // 2))
        f.write("};\n")

    print("已生成 %s" % OUT)
    for _, label, v, note, peak_db, rms_db in built:
        print("  %-5s %6d 帧（%5.0f ms） %6d 字节  峰值 %6.1f dBFS  RMS %6.1f dBFS   %s"
              % (label, len(v) // 2, len(v) / 2 * 1000.0 / RATE, len(v) * 2,
                 peak_db, rms_db, note))


if __name__ == "__main__":
    main()
