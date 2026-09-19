#!/usr/bin/env python3
"""生成 GB2312 -> Unicode 查表（main/app/gb2312_table.h / .c）。

为什么需要：网上下载的中文 .lrc 歌词很多是 GBK/GB2312 编码，而固件里
只按 UTF-8 处理文本。这张表就是给那种文件的转换依据（见 app_lyrics.c）。

表的形状是"区位码直查"：GB2312 的编码空间是 0xA1~0xF7 共 87 个区，
每区 0xA1~0xFE 共 94 个位，所以
    下标 = (高字节 - 0xA1) * 94 + (低字节 - 0xA1)
一次乘加就能查到，不需要二分。查不到的字（GBK 扩展区、没编码的空位）
填 0，由调用方决定怎么处理。

生成的是 C 源码而不是运行时解析的数据文件：固件不带文件系统镜像，
素材一律"编进固件"（和 tools/gen_splash_logo.py 一个路子）。

用法：
    python tools/gen_gb2312_table.py
"""

import os

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
OUT_H = os.path.join(ROOT, "main", "app", "gb2312_table.h")
OUT_C = os.path.join(ROOT, "main", "app", "gb2312_table.c")

HI_FIRST, HI_LAST = 0xA1, 0xF7
LO_FIRST, LO_LAST = 0xA1, 0xFE
COLS = LO_LAST - LO_FIRST + 1          # 94
ROWS = HI_LAST - HI_FIRST + 1          # 87
SIZE = ROWS * COLS


def build_table():
    """区位码 -> Unicode。Python 自带 gb2312 codec，不需要外部数据。"""
    table = [0] * SIZE
    for hi in range(HI_FIRST, HI_LAST + 1):
        for lo in range(LO_FIRST, LO_LAST + 1):
            try:
                ch = bytes([hi, lo]).decode("gb2312")
            except UnicodeDecodeError:
                continue               # 空位
            table[(hi - HI_FIRST) * COLS + (lo - LO_FIRST)] = ord(ch)
    return table


HEADER = """/*
 * gb2312_table.h — GB2312 -> Unicode 查表（**由脚本生成，不要手改**）
 *
 * 生成命令：python tools/gen_gb2312_table.py
 *
 * 下标 = (高字节 - 0xA1) * {cols} + (低字节 - 0xA1)，{rows} 个区 × {cols} 个位。
 * 值为 0 表示该位置没有对应字符（空位或 GBK 扩展区）。
 */
#pragma once

#include <stdint.h>

#define GB2312_TABLE_ROWS {rows}
#define GB2312_TABLE_COLS {cols}
#define GB2312_TABLE_SIZE (GB2312_TABLE_ROWS * GB2312_TABLE_COLS)

extern const uint16_t gb2312_unicode_table[GB2312_TABLE_SIZE];
"""


def write_header():
    with open(OUT_H, "w", encoding="utf-8", newline="\n") as f:
        f.write(HEADER.format(cols=COLS, rows=ROWS))


def write_source(table):
    mapped = sum(1 for v in table if v)
    with open(OUT_C, "w", encoding="utf-8", newline="\n") as f:
        f.write("/*\n")
        f.write(" * gb2312_table.c — GB2312 -> Unicode 查表（**由脚本生成，不要手改**）\n")
        f.write(" *\n")
        f.write(" * 生成命令：python tools/gen_gb2312_table.py\n")
        f.write(" * 数据源：Python 标准库的 gb2312 codec，共 %d 个区 × %d 个位，"
                "其中 %d 个位置有字。\n" % (ROWS, COLS, mapped))
        f.write(" * 占用 flash 约 %d 字节。\n" % (SIZE * 2))
        f.write(" */\n\n")
        f.write('#include "gb2312_table.h"\n\n')
        f.write("const uint16_t gb2312_unicode_table[GB2312_TABLE_SIZE] = {\n")
        per_line = 8
        for i in range(0, SIZE, per_line):
            chunk = table[i:i + per_line]
            f.write("    " + " ".join("0x%04X," % v for v in chunk) + "\n")
        f.write("};\n")


def main():
    table = build_table()
    write_header()
    write_source(table)
    print("已生成 %s" % os.path.relpath(OUT_H, ROOT))
    print("已生成 %s（%d 项，%d 项有字，%.1f KB）"
          % (os.path.relpath(OUT_C, ROOT), SIZE, sum(1 for v in table if v),
             SIZE * 2 / 1024.0))


if __name__ == "__main__":
    main()
