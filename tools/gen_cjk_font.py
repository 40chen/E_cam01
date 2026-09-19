#!/usr/bin/env python3
"""切出常用汉字子集，生成 main/ui/font_cjk_20.c / .h。

为什么需要：LVGL 自带的 `lv_font_simsun_16_cjk` 只有 1166 个常用字，实测中文
歌名和歌词**约一半显示成方框**。要覆盖正常中文得用 GB2312 全字库（6763 字）。

字从哪来：立创官方例程 14-handheld 自带一个 AlibabaPuHuiTi（阿里巴巴普惠体
3.45 Light，可商用）20px 全字库。但它有 2.37MB 位图 / 29197 个字形，编不进
4MB 的应用分区。

所以这个脚本**只做切片**，不做栅格化、不做格式转换：解析那个 C 数组里的
glyph_bitmap / glyph_dsc / cmaps，挑出需要的字形重新打包，输出仍然是 LVGL
原生字体格式（和 lv_font_conv 的输出同构），所以不存在"格式写错"的风险。

用法：
    python tools/gen_cjk_font.py

输入的默认路径指向参考工程。换机器或参考工程挪了位置时，用第一个参数指定。
输出是生成物，**要提交进库**（和 splash_logo.c 一样），这样没有参考工程也能编译。
"""

import os
import re
import sys

SRC_DEFAULT = r"O:/Desktop/DIY/ESP32 参考项目/14-handheld/main/assets/font_alipuhui20.c"
ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
OUT_C = os.path.join(ROOT, "main", "ui", "font_cjk_20.c")
OUT_H = os.path.join(ROOT, "main", "ui", "font_cjk_20.h")

FONT_NAME = "font_cjk_20"
# lv_font_fmt_txt_glyph_dsc_t 的 bitmap_index 是 20 位 → 位图总量不能超过 1MB
BITMAP_LIMIT = (1 << 20) - 1
# 相邻码位间隔超过这个值就另起一个 cmap 段（段越紧凑，二分查找越快，
# 而且 range_length 是 uint16，跨度必须 < 65536）
RANGE_GAP = 256
RANGE_SPAN = 60000


def parse_font(src):
    """把源 C 文件里的字体数据全部读出来。"""
    m = re.search(r"glyph_bitmap\[\] = \{(.*?)\n\};", src, re.S)
    if not m:
        raise SystemExit("找不到 glyph_bitmap[]，源文件格式变了？")
    # 注意 {1,2} 而不是 {2}：lv_font_conv 把小于 16 的字节写成 0x4 / 0x0 这种
    # **单位十六进制**（4bpp 打包出来的数据里这种值非常多），只认两位会把它们
    # 全部漏掉，位图整体错位、长度少一半——症状是每个汉字都渲染成乱码。
    bitmap = bytes(int(x, 16) for x in re.findall(r"0x([0-9a-fA-F]{1,2})", m.group(1)))
    if any(b > 0xFF for b in bitmap):
        raise SystemExit("位图里出现了超过一字节的值，源文件格式变了")

    m = re.search(r"glyph_dsc\[\] = \{(.*?)\n\};", src, re.S)
    if not m:
        raise SystemExit("找不到 glyph_dsc[]")
    glyphs = []
    pat = (r"\{\.bitmap_index = (\d+), \.adv_w = (\d+), \.box_w = (\d+), "
           r"\.box_h = (\d+), \.ofs_x = (-?\d+), \.ofs_y = (-?\d+)\}")
    for e in re.finditer(pat, m.group(1)):
        glyphs.append(tuple(int(g) for g in e.groups()))
    if not glyphs:
        raise SystemExit("glyph_dsc[] 一条都没解出来")

    # bitmap_index 必须单调不减，否则"下一个字形的起点"这种算法不成立
    idx = [g[0] for g in glyphs]
    if idx != sorted(idx):
        raise SystemExit("glyph_dsc 的 bitmap_index 不是递增的，解析方式要改")

    ulists = {}
    for name, body in re.findall(r"(unicode_list_\d+)\[\] = \{(.*?)\n\};", src, re.S):
        ulists[name] = [int(x, 16) for x in re.findall(r"0x([0-9a-fA-F]+)", body)]
    gofs = {}
    for name, body in re.findall(r"(glyph_id_ofs_list_\d+)\[\] = \{(.*?)\n\};", src, re.S):
        gofs[name] = [int(x) for x in re.findall(r"\d+", body)]

    m = re.search(r"static const lv_font_fmt_txt_cmap_t cmaps\[\] =\s*\{(.*?)\n\};", src, re.S)
    if not m:
        raise SystemExit("找不到 cmaps[]")
    cmaps = []
    for e in re.finditer(r"\{([^}]*)\}", m.group(1), re.S):
        body = e.group(1)

        def field(name):
            mm = re.search(r"\.%s = ([^,\n]+)" % name, body)
            return mm.group(1).strip() if mm else None

        cmaps.append({
            "range_start": int(field("range_start")),
            "range_length": int(field("range_length")),
            "glyph_id_start": int(field("glyph_id_start")),
            "unicode_list": field("unicode_list"),
            "glyph_id_ofs_list": field("glyph_id_ofs_list"),
            "list_length": int(field("list_length")),
            "type": field("type"),
        })

    def num(name, default=0):
        mm = re.search(r"\.%s = (-?\d+)" % name, src)
        return int(mm.group(1)) if mm else default

    meta = {
        "line_height": num("line_height", 28),
        "base_line": num("base_line", 6),
        "bpp": num("bpp", 4),
    }
    return bitmap, glyphs, ulists, gofs, cmaps, meta


def build_cp2gid(cmaps, ulists, gofs):
    """码位 → 字形 id。三种 cmap 类型都要认（源字体里三种都出现了）。"""
    cp2gid = {}
    for c in cmaps:
        rs, rl, gs = c["range_start"], c["range_length"], c["glyph_id_start"]
        t = c["type"]
        if t.endswith("FORMAT0_TINY"):
            for i in range(rl):
                cp2gid[rs + i] = gs + i
        elif t.endswith("FORMAT0_FULL"):
            lst = gofs[c["glyph_id_ofs_list"]]
            for i in range(min(len(lst), rl)):
                cp2gid[rs + i] = gs + lst[i]
        elif t.endswith("SPARSE_TINY"):
            for i, ofs in enumerate(ulists[c["unicode_list"]]):
                cp2gid[rs + ofs] = gs + i
        else:
            raise SystemExit("没见过的 cmap 类型：%s" % t)
    return cp2gid


def target_codepoints():
    """
    要保留哪些码位：ASCII + GB2312 符号区 + **一级汉字** + 常用中文标点/全角符号。

    为什么不要二级汉字（0xD8~0xF7，那 3008 个）：**装不下**。
    lv_font_fmt_txt_glyph_dsc_t 的 bitmap_index 只有 20 位，位图总量上限 1MB。
    实测一级字库约 700KB、加上二级就 1.25MB，直接越界（不是"慢"或者"浪费"，
    是写出来的索引根本表达不了）。
    一级字库 3755 字覆盖日常文本约 99.7%（按字频），歌词和歌名够用；
    真要补二级，得再生成一个字体用 lv_font_t.fallback 串起来。
    """
    cps = set(range(0x20, 0x7F))

    # GB2312 的 1~9 区是符号（含中文标点、假名、希腊/西里尔字母），
    # 16~55 区是一级汉字（按拼音排序的 3755 个常用字）
    for hi in range(0xA1, 0xD8):
        for lo in range(0xA1, 0xFF):
            try:
                ch = bytes([hi, lo]).decode("gb2312")
            except UnicodeDecodeError:
                continue
            cps.add(ord(ch))

    # 中文排版常用的几段：破折号/引号、CJK 标点、全角字母数字
    for lo, hi in ((0x2010, 0x203B), (0x3000, 0x303F), (0xFF01, 0xFF5E)):
        cps.update(range(lo, hi + 1))
    return cps


def group_ranges(cps):
    """把码位切成若干段，每段的跨度 < 65536（range_length 是 uint16）。"""
    ranges = []
    cur = []
    for cp in cps:
        if cur and (cp - cur[0]) > RANGE_SPAN:
            ranges.append(cur)
            cur = []
        cur.append(cp)
    if cur:
        ranges.append(cur)
    return ranges


def emit(src_path, bitmap, glyphs, cp2gid, meta):
    cps = target_codepoints()
    chosen = [(cp, cp2gid[cp]) for cp in sorted(cps) if cp in cp2gid]
    missing = [cp for cp in sorted(cps) if cp not in cp2gid]
    if not chosen:
        raise SystemExit("一个字都没匹配上，源字体和预期不符")

    # 新字体的字形顺序：id 0 保留，之后按码位升序排列（每个 cmap 段内连续）
    new_bitmap = bytearray()
    new_dsc = [(0, 0, 0, 0, 0, 0)]        # id 0：保留的空字形
    ranges = group_ranges([cp for cp, _ in chosen])

    by_cp = dict(chosen)
    cmap_entries = []
    gid = 1
    for rng in ranges:
        start = rng[0]
        ofs_list = []
        for cp in rng:
            src_gid = by_cp[cp]
            bi, adv, bw, bh, ox, oy = glyphs[src_gid]
            # 源字体里下一个字形的起点就是这个字形的终点（已校验过递增）
            end = glyphs[src_gid + 1][0] if src_gid + 1 < len(glyphs) else len(bitmap)
            new_dsc.append((len(new_bitmap), adv, bw, bh, ox, oy))
            new_bitmap += bitmap[bi:end]
            ofs_list.append(cp - start)
            gid += 1
        cmap_entries.append({
            "range_start": start,
            "range_length": rng[-1] - start,
            "glyph_id_start": gid - len(rng),
            "list": ofs_list,
        })

    if len(new_bitmap) > BITMAP_LIMIT:
        raise SystemExit("位图 %d 字节超过 bitmap_index 的 20 位上限" % len(new_bitmap))

    with open(OUT_H, "w", encoding="utf-8", newline="\n") as f:
        f.write("""/*
 * %s.h — 常用汉字字体（**由脚本生成，不要手改**）
 *
 * 生成命令：python tools/gen_cjk_font.py
 * 数据来源：立创官方例程 14-handheld 自带的 AlibabaPuHuiTi-3-45-Light（阿里巴巴
 * 普惠体，可商用），用脚本切成 GB2312 全字库 + ASCII + 常用中文标点的子集。
 *
 * 覆盖：%d 个字形（汉字 %d 个），位图 %d 字节。
 * 界面用 UI_FONT_CJK 引用它，见 ui_theme.h。
 */
#pragma once

#include "lvgl.h"

LV_FONT_DECLARE(%s);
""" % (FONT_NAME, len(new_dsc) - 1, len([c for c in chosen if c[0] > 0x2E7F]),
       len(new_bitmap), FONT_NAME))

    with open(OUT_C, "w", encoding="utf-8", newline="\n") as f:
        f.write("/*\n")
        f.write(" * %s.c — 常用汉字字体（**由脚本生成，不要手改**）\n" % FONT_NAME)
        f.write(" *\n")
        f.write(" * 生成命令：python tools/gen_cjk_font.py\n")
        f.write(" * 数据来源：%s\n" % os.path.basename(src_path))
        f.write(" *           （立创官方例程 14-handheld 自带的阿里巴巴普惠体 3.45 Light，可商用）\n")
        f.write(" *\n")
        f.write(" * 只做了切片：字形位图和字形描述都原样搬过来，重排了码表和索引，\n")
        f.write(" * 没有做栅格化或格式转换。kerning 丢弃（中文用不上，省得重排 kern 表）。\n")
        f.write(" *\n")
        f.write(" * 字形 %d 个（汉字 %d 个），%d 个 cmap 段，位图 %d 字节。\n"
                % (len(new_dsc) - 1, len([c for c in chosen if c[0] > 0x2E7F]),
                   len(cmap_entries), len(new_bitmap)))
        f.write(" */\n\n")
        f.write('#include "lvgl.h"\n\n')
        f.write("#ifndef FONT_CJK_20\n#define FONT_CJK_20 1\n#endif\n\n#if FONT_CJK_20\n\n")

        f.write("/*-----------------\n *    BITMAPS\n *----------------*/\n\n")
        f.write("static LV_ATTRIBUTE_LARGE_CONST const uint8_t glyph_bitmap[] = {\n")
        for i in range(0, len(new_bitmap), 16):
            f.write("    " + " ".join("0x%02X," % b for b in new_bitmap[i:i + 16]) + "\n")
        f.write("};\n\n")

        f.write("/*-----------------\n *  GLYPH DESCRIPTIONS\n *----------------*/\n\n")
        f.write("static const lv_font_fmt_txt_glyph_dsc_t glyph_dsc[] = {\n")
        for bi, adv, bw, bh, ox, oy in new_dsc:
            f.write("    {.bitmap_index = %d, .adv_w = %d, .box_w = %d, .box_h = %d, "
                    ".ofs_x = %d, .ofs_y = %d},\n" % (bi, adv, bw, bh, ox, oy))
        f.write("};\n\n")

        f.write("/*-----------------\n *  CHARACTER MAPPING\n *----------------*/\n\n")
        for i, ent in enumerate(cmap_entries):
            f.write("static const uint16_t unicode_list_%d[] = {\n" % i)
            for j in range(0, len(ent["list"]), 12):
                f.write("    " + ", ".join("0x%04X" % v for v in ent["list"][j:j + 12]) + ",\n")
            f.write("};\n\n")

        f.write("static const lv_font_fmt_txt_cmap_t cmaps[] = {\n")
        for i, ent in enumerate(cmap_entries):
            f.write("    {\n")
            f.write("        .range_start = %d, .range_length = %d, .glyph_id_start = %d,\n"
                    % (ent["range_start"], ent["range_length"], ent["glyph_id_start"]))
            f.write("        .unicode_list = unicode_list_%d, .glyph_id_ofs_list = NULL, "
                    ".list_length = %d,\n" % (i, len(ent["list"])))
            f.write("        .type = LV_FONT_FMT_TXT_CMAP_SPARSE_TINY\n    },\n")
        f.write("};\n\n")

        f.write("""static const lv_font_fmt_txt_dsc_t font_dsc = {
    .glyph_bitmap = glyph_bitmap,
    .glyph_dsc = glyph_dsc,
    .cmaps = cmaps,
    .kern_dsc = NULL,        /* 不用 kerning：中文无关，ASCII 只是略松一点 */
    .kern_scale = 0,
    .cmap_num = %d,
    .bpp = %d,
    .kern_classes = 0,
    .bitmap_format = 0,
    .cache = NULL,
};

const lv_font_t %s = {
    .get_glyph_dsc = lv_font_get_glyph_dsc_fmt_txt,
    .get_glyph_bitmap = lv_font_get_bitmap_fmt_txt,
    .line_height = %d,
    .base_line = %d,
    .subpx = LV_FONT_SUBPX_NONE,
    .underline_position = -1,
    .underline_thickness = 1,
    .dsc = &font_dsc,
    .fallback = NULL,
    .user_data = NULL,
};

#endif /*FONT_CJK_20*/
""" % (len(cmap_entries), meta["bpp"], FONT_NAME, meta["line_height"], meta["base_line"]))

    return len(new_dsc) - 1, len(new_bitmap), len(cmap_entries), missing


def main():
    src_path = sys.argv[1] if len(sys.argv) > 1 else SRC_DEFAULT
    if not os.path.exists(src_path):
        raise SystemExit("找不到源字体：%s\n（用第一个参数指定路径）" % src_path)

    with open(src_path, encoding="utf-8", errors="replace") as f:
        src = f.read()

    bitmap, glyphs, ulists, gofs, cmaps, meta = parse_font(src)
    print("源字体：%d 字形，位图 %.2f MB，%d 个 cmap 段"
          % (len(glyphs), len(bitmap) / 1048576, len(cmaps)))

    cp2gid = build_cp2gid(cmaps, ulists, gofs)
    print("源字体覆盖 %d 个码位" % len(cp2gid))

    n, nbytes, ncmap, missing = emit(src_path, bitmap, glyphs, cp2gid, meta)
    print("已生成 %s" % os.path.relpath(OUT_C, ROOT))
    print("  字形 %d 个，位图 %d 字节（%.0f KB，含 %d 个 cmap 段）"
          % (n, nbytes, nbytes / 1024, ncmap))
    if missing:
        print("  源字体里缺 %d 个目标码位（跳过），前几个：%s"
              % (len(missing), " ".join("U+%04X" % c for c in missing[:8])))


if __name__ == "__main__":
    main()
