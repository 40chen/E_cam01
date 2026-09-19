#!/usr/bin/env python3
"""把 main/ui/font_cjk_20.c 里的字形画成 ASCII，肉眼确认没串位。

为什么需要这个：字库是脚本切出来的，**切错了不会报错、也不会崩，只会显示成
乱码或方框**。2026-09-20 就踩过一次：解析源字体时正则只认两位十六进制
（`0x4f` 这种），而 lv_font_conv 把小于 16 的字节写成 `0x4` / `0x0` 这种
**单位**形式（4bpp 打包出来的数据里特别多），于是约一半的位图字节被静默丢掉，
整个位图错位——每个汉字都是乱码，而构建、烧录、日志一切正常。

用法：
    python tools/check_cjk_font.py [字库.c] [要看的字...]

不带参数时看几个常见字。除了画字形，还会检查每个字形的位图区间是否落在
位图数组内（越界同样是静默的，只会画错）。
"""
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DEFAULT_FONT = os.path.join(ROOT, "main", "ui", "font_cjk_20.c")


def load(path):
    src = open(path, encoding="utf-8").read()

    m = re.search(r"glyph_bitmap\[\] = \{(.*?)\n\};", src, re.S)
    bitmap = bytes(int(x, 16) for x in re.findall(r"0x([0-9a-fA-F]{1,2})", m.group(1)))

    m = re.search(r"glyph_dsc\[\] = \{(.*?)\n\};", src, re.S)
    dsc = [tuple(int(g) for g in e.groups()) for e in re.finditer(
        r"\{\.bitmap_index = (\d+), \.adv_w = (\d+), \.box_w = (\d+), \.box_h = (\d+), "
        r"\.ofs_x = (-?\d+), \.ofs_y = (-?\d+)\}", m.group(1))]

    m = re.search(r"static const lv_font_fmt_txt_cmap_t cmaps\[\] = \{(.*?)\n\};", src, re.S)
    cmaps = []
    for e in re.finditer(r"\{([^}]*)\}", m.group(1), re.S):
        body = e.group(1)

        def field(name):
            return re.search(r"\.%s = ([^,\n]+)" % name, body).group(1).strip()

        cmaps.append((int(field("range_start")), int(field("range_length")),
                      int(field("glyph_id_start")),
                      re.search(r"\.unicode_list = (\w+)", body).group(1)))

    ulists = {n: [int(x, 16) for x in re.findall(r"0x([0-9a-fA-F]+)", body)]
              for n, body in re.findall(r"(unicode_list_\d+)\[\] = \{(.*?)\n\};", src, re.S)}
    return bitmap, dsc, cmaps, ulists


def gid_of(cp, cmaps, ulists):
    """照 LVGL 的 SPARSE_TINY 查找方式（二分）找字形 id。"""
    for rs, rl, gs, lname in cmaps:
        rcp = cp - rs
        if rcp < 0 or rcp > rl:
            continue
        lst = ulists[lname]
        lo, hi = 0, len(lst) - 1
        while lo <= hi:
            mid = (lo + hi) // 2
            if lst[mid] == rcp:
                return gs + mid
            if lst[mid] < rcp:
                lo = mid + 1
            else:
                hi = mid - 1
    return None


def draw(bitmap, dsc, g):
    bi, adv, bw, bh, ox, oy = dsc[g]
    print("  gid=%d  %dx%d  ofs=(%d,%d)  adv=%d" % (g, bw, bh, ox, oy, adv))
    for y in range(bh):
        row = ""
        for x in range(bw):
            bit = y * bw + x
            byte = bitmap[bi + bit // 2]
            v = (byte >> 4) if bit % 2 == 0 else (byte & 0xF)
            row += " .:-=+*#@"[min(v, 8)]
        print("   " + row)


def main():
    args = sys.argv[1:]
    font = args[0] if args and args[0].endswith(".c") else DEFAULT_FONT
    chars = args[1:] if args and args[0].endswith(".c") else (args or ["李", "荣", "浩", "麻", "雀"])

    bitmap, dsc, cmaps, ulists = load(font)
    print("%s：位图 %d 字节，字形 %d 个（含保留的 gid 0），%d 个 cmap 段"
          % (os.path.relpath(font, ROOT), len(bitmap), len(dsc), len(cmaps)))

    # 越界检查：位图区间必须落在数组内，否则画出来是别处的数据（静默错字）
    bad = [i for i, (bi, adv, bw, bh, ox, oy) in enumerate(dsc)
           if bi + (bw * bh + 1) // 2 > len(bitmap)]
    if bad:
        print("**位图越界的字形：%d 个，前几个：%s**" % (len(bad), bad[:8]))
    else:
        print("位图区间检查通过（所有字形都落在数组内）")

    print("字形数：%d" % (len(dsc) - 1))
    for ch in chars:
        cp = ord(ch)
        g = gid_of(cp, cmaps, ulists)
        print("\n%s  U+%04X" % (ch, cp))
        if g is None:
            print("  字库里没有这个字")
            continue
        draw(bitmap, dsc, g)


if __name__ == "__main__":
    main()
