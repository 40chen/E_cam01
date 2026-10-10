#!/usr/bin/env python3
"""量 LVGL 内置字体里某个字符串占多少像素。

为什么需要它：这块屏只有 320 宽，界面里好几个地方是"算出来的固定尺寸"
（主菜单 94px 的格子、副屏页 148px 的栏宽）。**字宽靠估**会在这类地方翻车——
估宽了标签顶到边或者折行，估窄了白留一堆空隙，而且改一个字就得重新猜一遍。
LVGL 的字体文件里每个字形都带着 adv_w（advance width），直接读出来就是准确值。

用法：
    python tools/measure_font.py 20 "CPU" "12.3%" "Balance" "45.67"
    python tools/measure_font.py 16 "Tokens" "1.23M"

只依赖标准库。只支持 ASCII（内置 montserrat 的那段 cmap 是 format0_tiny，
`glyph_id = codepoint - range_start + glyph_id_start`，直接算得出来）；
非 ASCII 会报错——FontAwesome 那些符号在稀疏表里，要用得先把 unicode_list
也解出来，目前没这个需要。
"""

import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
FONT_DIR = os.path.join(ROOT, "managed_components", "lvgl__lvgl", "src", "font")


def load(font_name):
    """返回 {codepoint: 像素宽度}。"""
    path = os.path.join(FONT_DIR, font_name + ".c")
    if not os.path.exists(path):
        sys.exit("找不到字体源文件：%s" % path)
    src = open(path, encoding="utf-8").read()

    # 只认 ASCII 那一段的 cmap：range_start=32, glyph_id_start=1
    m = re.search(r"\.range_start = 32, \.range_length = (\d+), \.glyph_id_start = (\d+)", src)
    if not m:
        sys.exit("字体里没找到 ASCII 那段 cmap，格式变了？")
    length, id_start = int(m.group(1)), int(m.group(2))

    # glyph_dsc[] 的每个元素：{\n.bitmap_index = ..., .adv_w = N, ...}
    body = re.search(r"glyph_dsc\[\] = \{(.*?)\n\};", src, re.S)
    if not body:
        sys.exit("字体里没找到 glyph_dsc[]，格式变了？")
    adv = [int(x) for x in re.findall(r"\.adv_w = (\d+)", body.group(1))]

    # id 0 是保留项，id_start 对应 range_start
    table = {}
    for i in range(length):
        gid = id_start + i
        if gid >= len(adv):
            break
        # adv_w 是 4 位定点：>>4 得到整数像素
        table[32 + i] = adv[gid] / 16.0
    return table


def width(table, text):
    return sum(table.get(ord(c), 0.0) for c in text)


def main():
    if len(sys.argv) < 3:
        print(__doc__)
        return 1
    size = sys.argv[1]
    table = load("lv_font_montserrat_" + size)
    total = 0.0
    for s in sys.argv[2:]:
        w = width(table, s)
        print("  %-12s %6.1f px" % ('"%s"' % s, w))
        total = max(total, w)
    print("  %-12s %6.1f px" % ("(最宽)", total))
    return 0


if __name__ == "__main__":
    sys.exit(main())
