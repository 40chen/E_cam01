#!/usr/bin/env python3
"""把 assets/logo2.jpg 转成 LVGL 能直接显示的 C 数组（main/ui/splash_logo.c）。

为什么要转而不是让固件在启动时解码：
    启动页是开机后第一眼看到的东西，它的意义就是盖住启动那段时间。
    在显示之前先解一次 JPEG（几百毫秒）等于把这段空白又还回去了。
    预先转成裸像素放进 flash 最省事，代价是占 150KB 左右。

为什么留白处要复制边缘像素而不是填固定颜色：
    留白处沿用图片边缘颜色，可避免图片与留白之间出现明显的接缝。

用法：
    python tools/gen_splash_logo.py

依赖 ImageMagick（magick 命令）和 Python 3，不需要 Pillow。
换 logo 之后重新跑一次即可。
"""

import os
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SRC = os.path.join(ROOT, "assets", "logo2.jpg")
OUT = os.path.join(ROOT, "main", "ui", "splash_logo.c")

SCREEN_W, SCREEN_H = 320, 240
# 四周留白，图片不贴边。改这里就能调空隙大小。
MARGIN = 8
# 图片按原比例缩放，最大不超过这个可用区（不是拉伸填满）
BOX_W = SCREEN_W - 2 * MARGIN
BOX_H = SCREEN_H - 2 * MARGIN

BYTES_PER_LINE = 16


def load_logo():
    """先裁成居中正方形，再缩放到能放进 BOX_W x BOX_H 的最大尺寸，读回 RGB888。

    源图的图案位于画布中央；裁成最大居中正方形，可避免画布两侧的空白
    限制图案在屏幕上的显示尺寸。

    用 -resize WxH（不带 !）让 ImageMagick 按比例适配，实际尺寸从 PPM 头里读。
    """
    try:
        # 先问一下源图多大，好算出能放下的最大居中正方形
        dim = subprocess.run(["magick", "identify", "-format", "%w %h", SRC],
                             capture_output=True, check=True).stdout.split()
        crop = min(int(dim[0]), int(dim[1]))

        # -gravity center -crop NxN+0+0 取居中正方形；+repage 去掉裁剪留下的
        # 画布偏移，否则后续操作会带着这个偏移
        raw = subprocess.run(
            ["magick", SRC,
             "-gravity", "center", "-crop", "%dx%d+0+0" % (crop, crop),
             "+repage",
             "-resize", "%dx%d" % (BOX_W, BOX_H),
             "-depth", "8", "ppm:-"],
            capture_output=True, check=True,
        ).stdout
    except FileNotFoundError:
        sys.exit("找不到 magick 命令，请装 ImageMagick 并加入 PATH")
    except subprocess.CalledProcessError as e:
        sys.exit("ImageMagick 失败: %s" % e.stderr.decode(errors="replace"))

    # PPM P6 头是 "P6\n<w> <h>\n<maxval>\n"，中间可能夹注释，按 token 解析
    fields, pos = [], 0
    while len(fields) < 4:
        while pos < len(raw) and raw[pos:pos + 1].isspace():
            pos += 1
        if raw[pos:pos + 1] == b"#":
            while pos < len(raw) and raw[pos:pos + 1] != b"\n":
                pos += 1
            continue
        start = pos
        while pos < len(raw) and not raw[pos:pos + 1].isspace():
            pos += 1
        fields.append(raw[start:pos])
    pos += 1                     # 跳掉 maxval 之后的那个空白字符

    magic, w, h, maxval = fields[0], int(fields[1]), int(fields[2]), int(fields[3])
    if magic != b"P6" or maxval != 255:
        sys.exit("只支持 P6 / 8bit 的 PPM，拿到 %r maxval=%d" % (magic, maxval))

    pix = raw[pos:]
    if len(pix) != w * h * 3:
        sys.exit("像素长度不对：期望 %d，拿到 %d" % (w * h * 3, len(pix)))
    if w > BOX_W or h > BOX_H:
        sys.exit("缩放结果 %dx%d 超出可用区 %dx%d" % (w, h, BOX_W, BOX_H))
    return pix, w, h


def to565(r, g, b):
    """返回 LVGL 需要的两字节。

    CONFIG_LV_COLOR_16_SWAP=y 时 lv_color16_t 的位域是
    green_h:3, red:5, blue:5, green_l:3（小端从低位排），展开后两字节
    与标准 RGB565 相反，所以要存成大端：高字节在前。
    """
    v = ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3)
    return bytes((v >> 8, v & 0xFF))


def build_screen(pix, w, h):
    """把 w x h 的图居中放到 SCREEN_W x SCREEN_H 上，四周留白外扩边缘填充。

    留白处不填固定色，而是把坐标夹回图片范围内取最近的那个边缘像素。
    四个角会被夹到最靠近的那个角像素，同样连续。
    """
    ox = (SCREEN_W - w) // 2
    oy = (SCREEN_H - h) // 2
    out = bytearray(SCREEN_W * SCREEN_H * 2)

    for y in range(SCREEN_H):
        sy = min(max(y - oy, 0), h - 1)
        row = y * SCREEN_W * 2
        base = sy * w * 3
        for x in range(SCREEN_W):
            sx = min(max(x - ox, 0), w - 1)
            s = base + sx * 3
            out[row + x * 2: row + x * 2 + 2] = to565(pix[s], pix[s + 1], pix[s + 2])

    return out, ox, oy


def write_c(data, pix, w, h, ox, oy):
    body = []
    for i in range(0, len(data), BYTES_PER_LINE):
        body.append("    " + " ".join("0x%02x," % b
                                      for b in data[i:i + BYTES_PER_LINE]))

    # 取四个角，写进注释里，方便日后核对颜色有没有变
    def px(x, y):
        o = (y * w + x) * 3
        return "#%02x%02x%02x" % (pix[o], pix[o + 1], pix[o + 2])

    header = """/*
 * splash_logo.c — 启动页图片（**自动生成，不要手改**）
 *
 * 由 tools/gen_splash_logo.py 从 assets/logo2.jpg 生成：
 *   - 按原比例缩放到 {lw}x{lh}（能放进 {bw}x{bh} 的最大尺寸），居中放在
 *     {sw}x{sh} 画布上，即左右各留 {ox} 像素、上下各留 {oy} 像素
 *   - 留白处把坐标夹回图片范围内取最近的边缘像素（即向四周外扩边缘）。
 *     四角颜色：左上 {tl}、右上 {tr}、左下 {bl}、右下 {br}。
 *   - 转成 LVGL 需要的字节序：CONFIG_LV_COLOR_16_SWAP=y 时 lv_color16_t 的
 *     位域是 green_h:3, red:5, blue:5, green_l:3，展开后两字节与标准 RGB565
 *     相反，所以存大端（高字节在前）。
 *
 * 重新生成：python tools/gen_splash_logo.py
 */
#include "lvgl.h"

#include "splash_logo.h"

static const uint8_t splash_logo_map[{n}] = {{
""".format(lw=w, lh=h, bw=BOX_W, bh=BOX_H, sw=SCREEN_W, sh=SCREEN_H,
           ox=ox, oy=oy, n=len(data),
           tl=px(0, 0), tr=px(w - 1, 0), bl=px(0, h - 1), br=px(w - 1, h - 1))

    footer = """
}};

const lv_img_dsc_t splash_logo = {{
    .header.cf = LV_IMG_CF_TRUE_COLOR,
    .header.always_zero = 0,
    .header.w = {sw},
    .header.h = {sh},
    .data_size = {n},
    .data = splash_logo_map,
}};
""".format(sw=SCREEN_W, sh=SCREEN_H, n=len(data))

    with open(OUT, "w", encoding="utf-8", newline="\n") as f:
        f.write(header)
        f.write("\n".join(body))
        f.write("\n")
        f.write(footer)


def main():
    if not os.path.exists(SRC):
        sys.exit("找不到源图：%s" % SRC)
    pix, w, h = load_logo()
    data, ox, oy = build_screen(pix, w, h)
    write_c(data, pix, w, h, ox, oy)
    print("已生成 %s" % OUT)
    print("  logo 缩放到 %dx%d，居中于 %dx%d，左右留 %d、上下留 %d"
          % (w, h, SCREEN_W, SCREEN_H, ox, oy))


if __name__ == "__main__":
    main()
