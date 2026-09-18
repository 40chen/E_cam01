#!/usr/bin/env python3
"""把 assets/quick-camera-click.wav 转成固件里能直接播的 C 数组。

为什么要转、为什么不能在固件里解码：
    固件里没有 MP3/任何音频解码器（音乐页还没做，Helix / ESP-ADF 都没引入），
    所以只能预先解成裸 PCM 编进固件。和启动页图片是一个思路。

为什么要降采样到 16kHz，而且**必须正好是 16kHz**：
    体积是一方面：44.1kHz 立体声 16bit 一秒要 176KB，这段 158ms 就是 27.8KB。
    而 I2S 的 DMA 读不了 flash，播放前必须整个拷进内部 DMA 内存，在相机 +
    WiFi + LVGL 都跑着的时候长期占用这么多内部 RAM 有风险。16kHz 只要 10KB。

    但**采样率必须和 BSP 开机时打开的采样率一致（BSP_AUDIO_DEFAULT_RATE）**，
    这比体积重要得多。不一致的话 bsp_audio_set_fs() 会走 close/open 去切，
    而 esp_codec_dev 那条路径有 bug：

        close → es8311_suspend() 最后写 RESET_REG00=0x1F，bit7=0 把芯片按在复位态
        open  → ① set_fs  写 CLK_MANAGER REG02~REG08   ← 芯片还在复位态，写全丢
                ② enable  → es8311_start() 写 RESET_REG00=0x80 解除复位
                           但 es8311_start 根本不碰时钟分频器

    结果就是分频器停在旧采样率、I2S 跑新采样率 → 声音严重失真，而且这副烂
    状态会一直留着。改成 16kHz 之后 set_fs 从开机起就是空操作，一次都不切。

用法：
    python tools/gen_shutter_sound.py

只依赖 Python 3 标准库（wave / array），不需要 numpy，也不需要任何外部工具。
换音效之后重新跑一次即可。
"""

import math
import os
import sys
import wave
from array import array

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SRC = os.path.join(ROOT, "assets", "quick-camera-click.wav")
OUT = os.path.join(ROOT, "main", "ui", "shutter_sound.c")

OUT_RATE = 16000            # 必须等于 lckfb_bsp.h 的 BSP_AUDIO_DEFAULT_RATE
BYTES_PER_LINE = 16

# 抗混叠低通的抽头数和截止比例。截止取 OUT_RATE 的 45%，给插值留过渡带，
# 避免奈奎斯特附近鼓包。63 抽头大致能把截止点以上压 40dB 以上，音效够用。
LP_TAPS = 63
LP_CUTOFF_RATIO = 0.45

# 软限幅阈值，相对峰值。越小压得越狠、响度越高，但过头就变成"嗡"不是"咔"。
# 0.4 大约换来 +8dB 的 RMS。
LIMIT_RATIO = 0.4

# 低中频"身体"的参数。频率贴着这颗小喇叭的谐振区（几百 Hz），衰减留一点尾巴。
#
# BODY_LEVEL 是"身体"相对原素材峰值的幅度，也是最需要权衡的一个（实测能量占比）：
#   0.30 → 1kHz 以下 60%，3-8kHz 还留 18%   ← 用这个，快门感还在
#   0.50 → 81% / 9%
#   0.75 → 91% / 5%，基本就是一声"咚"，不像快门了
# 改这个值之前先跑 tools/preview_shutter_body.py，到 assets/ 下试听几档再定。
#
# 总长会变成 158 + 92 ≈ 250ms——**别超过 256ms**：250ms 立体声 16bit 是 15988
# 字节，而实测一次性能够拿到的连续内部 DMA 内存上限就在 16KB 出头
# （要 96000 字节会直接分配失败，见 page_camera.c 里的注释）。
BODY_FREQ_HZ = 480.0
BODY_DECAY_MS = 92.0
BODY_LEVEL = 0.30


def add_body(left, right, rate, freq, decay_ms, level):
    """给高频的"嗒"叠一层低中频的"身体"（阻尼正弦）。

    **为什么必须要做**——实测的频谱（1024 点 DFT，能量占比）：
                   源素材    固件里这段
        0-  250Hz    0.0%      0.1%
      250-  500Hz    0.1%      0.1%
      500- 1000Hz    0.5%      1.4%
     1000- 2000Hz    4.7%      6.2%
     2000- 4000Hz   25.2%     57.3%
     4000- 8000Hz   28.4%     34.8%
     8000-22050Hz   41.2%        —

    1kHz 以下只有 1.5% 的能量，92% 落在 2~8kHz。而这块板子那个小喇叭
    3~5kHz 以上就滚降了——**等于 92% 的能量都落在它放不出来的频段**。
    对照实验：Settings→Test Speaker 放 440Hz 正弦很清楚（能量全在喇叭谐振点上），
    快门声在同一套代码、同一条通路上却几乎听不见。所以这不是软件问题。

    做法是叠一个指数衰减的正弦——机械快门"咔哒"里的那个"哒"本来就是这东西。
    频率贴近小喇叭的谐振区，衰减留出一点尾巴让听感结实而不是"噗"。
    """
    n = max(len(left), len(right))

    src_peak = 0.0
    for v in left:
        src_peak = max(src_peak, abs(v))
    for v in right:
        src_peak = max(src_peak, abs(v))
    if src_peak <= 0.0:
        return left, right

    tail = int(rate * decay_ms / 1000.0)
    total = n + tail
    tau = (decay_ms / 1000.0) / 4.6            # 到 decay_ms 时衰减到约 1%
    g = level * src_peak

    out_l = [0.0] * total
    out_r = [0.0] * total
    for i in range(total):
        t = i / rate
        b = math.sin(2.0 * math.pi * freq * t) * math.exp(-t / tau) * g
        out_l[i] = (left[i] if i < n else 0.0) + b
        out_r[i] = (right[i] if i < n else 0.0) + b
    return out_l, out_r


def soft_limit(left, right):
    """tanh 软限幅。

    为什么要做：源素材峰值 -2.3 dBFS，RMS 只有 -22.4 dBFS，波峰因数 20dB。
    人耳听的是 RMS 不是峰值，所以哪怕归一化到满量程，这个尖脉冲听着还是轻。
    把峰值用 tanh 压下来再整体抬回去，等于把能量往上搬：峰值不变，RMS 涨约
    8dB。比直接加大数字音量好——后者是硬削波，声音会变"炸"。

    副作用是"咔"的最尖那一下会稍微变钝，对快门声来说反而更接近真实相机。
    """
    peak = 0.0
    for v in left:
        peak = max(peak, abs(v))
    for v in right:
        peak = max(peak, abs(v))
    if peak <= 0.0:
        return left, right

    t = peak * LIMIT_RATIO
    return ([t * math.tanh(v / t) for v in left],
            [t * math.tanh(v / t) for v in right])


def load_wav():
    """读源文件，返回 ([左声道], [右声道], 采样率)，样本是 float。"""
    if not os.path.exists(SRC):
        sys.exit("找不到源文件：%s" % SRC)
    with wave.open(SRC, "rb") as w:
        ch = w.getnchannels()
        width = w.getsampwidth()
        rate = w.getframerate()
        n = w.getnframes()
        raw = w.readframes(n)

    if width != 2:
        sys.exit("只支持 16bit PCM，拿到 %d 字节/样本" % width)
    if ch not in (1, 2):
        sys.exit("只支持单声道或立体声，拿到 %d 声道" % ch)

    a = array("h")
    a.frombytes(raw)
    if sys.byteorder == "big":              # WAV 是小端
        a.byteswap()

    left = [float(v) for v in a[0::ch]]
    right = [float(v) for v in a[1::ch]] if ch == 2 else list(left)
    return left, right, rate


def lowpass(x, rate, cutoff_hz):
    """窗函数 sinc 低通（Hamming）。

    抽取前必须把 OUT_RATE/2 以上的成分压掉，否则会折叠回可听频段变成"沙沙"
    的怪声。这里用 63 抽头，够把 7.2kHz 以上压掉 40dB 以上。
    """
    n = LP_TAPS                             # 奇数，保证线性相位
    half = n // 2
    fc = cutoff_hz / rate                   # 归一化截止频率（周期/样本）
    h = []
    for i in range(n):
        t = i - half
        arg = 2.0 * math.pi * fc * t
        # sinc(2*fc*t) = sin(2*pi*fc*t) / (2*pi*fc*t)，t=0 处取极限 1
        sinc = 1.0 if t == 0 else math.sin(arg) / arg
        win = 0.54 - 0.46 * math.cos(2.0 * math.pi * i / (n - 1))
        h.append(sinc * win)

    total = sum(h)
    h = [v / total for v in h]

    out = [0.0] * len(x)
    for i in range(len(x)):
        acc = 0.0
        base = i - half
        for k in range(n):
            j = base + k
            if 0 <= j < len(x):
                acc += x[j] * h[k]
        out[i] = acc
    return out


def resample(left, right, rate):
    """先低通，再线性插值降到 OUT_RATE。"""
    if rate == OUT_RATE:
        return left, right

    left = lowpass(left, rate, OUT_RATE * LP_CUTOFF_RATIO)
    right = lowpass(right, rate, OUT_RATE * LP_CUTOFF_RATIO)

    n_in = len(left)
    n_out = int(round(n_in * OUT_RATE / rate))
    if n_out < 2:
        return [0.0], [0.0]
    step = (n_in - 1) / float(n_out - 1)

    def interp(x):
        out = [0.0] * n_out
        for i in range(n_out):
            t = i * step
            j = int(t)
            if j >= n_in - 1:
                out[i] = x[-1]
                continue
            f = t - j
            out[i] = x[j] * (1.0 - f) + x[j + 1] * f
        return out

    return interp(left), interp(right)


def to_stereo_int16(left, right):
    """按全局峰值归一化，转成交错的 int16：[L0, R0, L1, R1, ...]"""
    peak = 0.0
    for v in left:
        peak = max(peak, abs(v))
    for v in right:
        peak = max(peak, abs(v))

    # 必须能放大，不能只衰减。这里原来写的是 min(1.0, 32767.0/peak)，
    # 那个 clamp 让归一化实际从未生效过：源文件峰值 -2.3 dBFS，输出就一直是
    # -2.3 dBFS，白丢 2.3dB。软限幅把样本压低之后更离谱——峰值只剩 -12.6 dBFS。
    g = 32767.0 / peak if peak > 0 else 1.0
    v = array("h")
    for i in range(max(len(left), len(right))):
        for ch in (left, right):
            x = ch[i] if i < len(ch) else 0.0
            s = x * g
            s = 32767 if s > 32767 else (-32768 if s < -32768 else s)
            v.append(int(s + 0.5) if s >= 0 else int(s - 0.5))
    if sys.byteorder == "big":
        v.byteswap()
    return v


def write_c(samples, rate, src_rate):
    data = samples.tobytes()
    lines = []
    for i in range(0, len(data), BYTES_PER_LINE):
        lines.append("    " + " ".join("0x%02x," % b
                                       for b in data[i:i + BYTES_PER_LINE]))

    frames = len(samples) // 2
    dur_ms = frames * 1000.0 / rate

    with open(OUT, "w", encoding="utf-8", newline="\n") as f:
        f.write("""/*
 * shutter_sound.c — 快门声 PCM（**自动生成，不要手改**）
 *
 * 由 tools/gen_shutter_sound.py 从 assets/quick-camera-click.wav 生成：
 *   - {rate} Hz / 16bit / 立体声交错（单声道源复制成两声道）
 *   - {frames} 帧，约 {dur:.0f} ms，共 {n} 字节
 *   - 源文件是 {srcrate} Hz，这里重采样过：I2S 的 DMA 读不了 flash，播放前
 *     必须整个拷进内部 DMA 内存，体积小能少占内部 RAM。
 *   - 采样率**必须**等于 BSP_AUDIO_DEFAULT_RATE，原因见脚本头部注释
 *
 * 重新生成：python tools/gen_shutter_sound.py
 */
#include <stdint.h>

#include "shutter_sound.h"

static const uint8_t shutter_sound_pcm[{n}] = {{
""".format(rate=rate, frames=frames, dur=dur_ms, n=len(data), srcrate=src_rate))
        f.write("\n".join(lines))
        f.write("""
}};

const shutter_sound_t shutter_sound = {{
    .pcm    = shutter_sound_pcm,
    .bytes  = {n},
    .rate   = {rate},
    .frames = {frames},
}};
""".format(n=len(data), rate=rate, frames=frames))


def main():
    left, right, src_rate = load_wav()
    left, right = resample(left, right, src_rate)
    left, right = add_body(left, right, OUT_RATE, BODY_FREQ_HZ, BODY_DECAY_MS, BODY_LEVEL)
    left, right = soft_limit(left, right)
    v = to_stereo_int16(left, right)
    write_c(v, OUT_RATE, src_rate)
    frames = len(v) // 2
    print("已生成 %s" % OUT)
    print("  源 %d Hz → %d Hz / 16bit / 立体声，%d 帧（%.0f ms），%d 字节"
          % (src_rate, OUT_RATE, frames, frames * 1000.0 / OUT_RATE, len(v) * 2))


if __name__ == "__main__":
    main()
