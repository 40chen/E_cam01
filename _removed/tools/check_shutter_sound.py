#!/usr/bin/env python3
"""把"源 WAV"和"固件里实际会播的 PCM"分别导成 WAV，用来在电脑上对比试听。

为什么要这个：
    板子上听不到声音时，有两类可能——(a) 生成脚本把音效转坏了，(b) 转得没问题
    而是固件播放那一侧的问题。这两类靠串口日志分不开，但**在电脑上放一遍就能
    分开**：如果导出的 check_firmware.wav 听着是正常的"咔"，那数据就是好的，
    问题必在固件；如果它听着就不对，那是脚本的问题，改板子没用。

用法：
    python tools/check_shutter_sound.py

在 assets/ 下生成两个文件，双击即可播放：
    check_source.wav     —— assets/quick-camera-click.wav 原样
    check_firmware.wav   —— 从 main/ui/shutter_sound.c 里解出来的、固件真正播放的东西

输出放在 assets/ 而不是工程根目录：这个脚本每跑一次就覆盖一次，
散在根目录既乱又容易和真正的素材搞混（曾经就搞混过一次——以为
check_firmware.wav 还是自己上次听过的那版，其实已经被覆盖了）。
"""

import os
import re
import sys
import wave
from array import array

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
ASSETS = os.path.join(ROOT, "assets")
SRC = os.path.join(ASSETS, "quick-camera-click.wav")
GEN = os.path.join(ROOT, "main", "ui", "shutter_sound.c")
OUT_SRC = os.path.join(ASSETS, "check_source.wav")
OUT_FW = os.path.join(ASSETS, "check_firmware.wav")


def stats(samples, rate):
    peak = max(abs(v) for v in samples) if samples else 0
    import math
    rms = math.sqrt(sum(float(v) * v for v in samples) / len(samples)) if samples else 0.0
    db = lambda x: (20 * math.log10(x / 32768.0)) if x > 0 else float("-inf")
    return ("%d Hz, %d 帧, %.0f ms, 峰值 %.1f dBFS, RMS %.1f dBFS"
            % (rate, len(samples), len(samples) * 1000.0 / rate, db(peak), db(rms)))


def write_wav(path, samples, rate, channels):
    with wave.open(path, "wb") as w:
        w.setnchannels(channels)
        w.setsampwidth(2)
        w.setframerate(rate)
        w.writeframes(samples.tobytes())
    print("  -> %s" % path)


def export_source():
    with wave.open(SRC, "rb") as w:
        ch, width, rate, n = w.getnchannels(), w.getsampwidth(), w.getframerate(), w.getnframes()
        raw = w.readframes(n)
    a = array("h")
    a.frombytes(raw)
    if sys.byteorder == "big":
        a.byteswap()
    left = a[0::ch]
    print("源文件 %s" % os.path.basename(SRC))
    print("  " + stats(left, rate))
    # 统一导成单声道，方便和下面那个对比
    mono = array("h", left)
    if sys.byteorder == "big":
        mono.byteswap()
    write_wav(OUT_SRC, mono, rate, 1)


def export_firmware():
    """从生成的 C 文件里把字节抠出来。里面应该**没有** RIFF 头——
    有的话就说明脚本把 WAV 头也当样本写进去了，那正是爆音的来源。"""
    text = open(GEN, encoding="utf-8").read()
    body = text.split("shutter_sound_pcm[", 1)[1]
    size = int(body.split("]", 1)[0])
    body = body.split("] = {", 1)[1].split("};", 1)[0]
    data = bytes(int(b, 16) for b in re.findall(r"0x([0-9a-f]{2})", body))

    # 采样率从结构体里读，别写死
    m = re.search(r"\.rate\s*=\s*(\d+)", text)
    rate = int(m.group(1)) if m else 16000

    print("固件 PCM %s" % os.path.basename(GEN))
    print("  声明 %d 字节，实际抠出 %d 字节" % (size, len(data)))
    if data[:4] == b"RIFF":
        print("  ** 里面有 RIFF 头！脚本没剥干净，这就是爆音来源 **")
    else:
        print("  前 4 字节 %s，不是 RIFF —— 头已经剥掉了" % data[:4].hex(" "))
    if len(data) % 4 != 0:
        print("  ** 字节数不是 4 的倍数（16bit 立体声），样本会错位 **")

    a = array("h")
    a.frombytes(data)
    if sys.byteorder == "big":
        a.byteswap()
    left = a[0::2]                    # 交错立体声，取左声道
    print("  " + stats(left, rate))
    mono = array("h", left)
    if sys.byteorder == "big":
        mono.byteswap()
    write_wav(OUT_FW, mono, rate, 1)


def main():
    print("=== 1/2 源文件 ===")
    export_source()
    print("\n=== 2/2 固件里实际会播的 ===")
    export_firmware()
    print("\n两个文件都在工程根目录，双击试听。")
    print("  check_firmware.wav 听着是正常的\"咔\" → 数据没问题，问题在固件播放那一侧")
    print("  听着不对/没声/是噪音           → 问题在生成脚本，板子上怎么调都没用")


if __name__ == "__main__":
    main()
