#!/usr/bin/env python3
"""生成几档不同"低中频身体"强度的快门声，放在 assets/ 下试听。

为什么需要这个工具：
    `gen_shutter_sound.py` 里的 BODY_LEVEL 是整条音频链上唯一需要**主观权衡**
    的参数（其它步骤都是确定性的）。它决定"咔"在板子上能不能被那颗小喇叭放出来：

        body 小 → 像快门，但在板子上听不见
        body 大 → 在板子上很响，但变成一声"咚"

    而这个权衡在电脑上试听和在板子上听，结论是不一样的（电脑/耳机能重放
    8kHz 以上，小喇叭不行）。所以换 BODY_LEVEL 之前，建议先跑这个脚本，
    在**耳机**上快速过一遍，定个大致范围，再上板子复核。

用法：
    python tools/preview_shutter_body.py

在 assets/ 下生成 shutter-body-<档位>.wav，直接双击试听。
注意这些只是试听文件，不会被编进固件；固件里用的是 BODY_LEVEL 那一个值。
"""

import os
import sys
import wave

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import gen_shutter_sound as g

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
ASSETS = os.path.join(ROOT, "assets")

LEVELS = (0.10, 0.30, 0.50, 0.75)


def band_share(x, rate, lo, hi):
    """[lo, hi) 频段占总能量的比例，%。用 1024 点 DFT，取能量最大的那一段。"""
    import cmath
    import math

    win = 1024
    best, off = -1.0, 0
    for s in range(0, max(1, len(x) - win), 128):
        e = sum(float(v) * v for v in x[s:s + win])
        if e > best:
            best, off = e, s
    seg = [float(v) for v in x[off:off + win]]
    seg = [seg[i] * (0.5 - 0.5 * math.cos(2 * math.pi * i / (win - 1))) for i in range(win)]
    mags = []
    for k in range(win // 2 + 1):
        acc = 0j
        for i in range(win):
            acc += seg[i] * cmath.exp(-2j * math.pi * k * i / win)
        mags.append(abs(acc))
    tot = sum(m * m for m in mags) or 1.0
    return sum(mags[k] * mags[k] for k in range(win // 2 + 1)
               if lo <= k * rate / win < min(hi, rate / 2)) / tot * 100.0


def main():
    base_l, base_r, src_rate = g.load_wav()
    base_l, base_r = g.resample(base_l, base_r, src_rate)

    print("源素材 %d Hz → %d Hz，下面每档都会做同样的限幅和归一化：\n"
          % (src_rate, g.OUT_RATE))
    for lvl in LEVELS:
        l, r = g.add_body(list(base_l), list(base_r), g.OUT_RATE,
                          g.BODY_FREQ_HZ, g.BODY_DECAY_MS, lvl)
        l, r = g.soft_limit(l, r)
        v = g.to_stereo_int16(l, r)

        path = os.path.join(ASSETS, "shutter-body-%.2f.wav" % lvl)
        w = wave.open(path, "wb")
        w.setnchannels(2)
        w.setsampwidth(2)
        w.setframerate(g.OUT_RATE)
        w.writeframes(v.tobytes())
        w.close()

        mono = list(v[0::2])
        print("  shutter-body-%.2f.wav  %d 帧 %.0f ms  |  <1kHz %2.0f%%  "
              "1-3kHz %2.0f%%  3-8kHz %2.0f%%%s"
              % (lvl, len(v) // 2, len(v) / 2 * 1000.0 / g.OUT_RATE,
                 band_share(mono, g.OUT_RATE, 0, 1000),
                 band_share(mono, g.OUT_RATE, 1000, 3000),
                 band_share(mono, g.OUT_RATE, 3000, 8000),
                 "   <- 当前 BODY_LEVEL" if abs(lvl - g.BODY_LEVEL) < 1e-9 else ""))

    print("\n都在 assets/ 下。定好档位后去 gen_shutter_sound.py 改 BODY_LEVEL，"
          "再跑一次它。")


if __name__ == "__main__":
    main()
