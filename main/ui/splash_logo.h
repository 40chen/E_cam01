/*
 * splash_logo.h — 启动页图片
 *
 * 定义在 splash_logo.c 里，那个文件是 tools/gen_splash_logo.py 从
 * assets/logo2.jpg 生成的，不要手改。换 logo 就重新跑一次脚本。
 *
 * 图像是 320x240 的 TRUE_COLOR，已经按 CONFIG_LV_COLOR_16_SWAP=y 排好
 * 字节序，可以直接 lv_img_set_src() 用，不需要任何解码。
 */
#pragma once

#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

extern const lv_img_dsc_t splash_logo;

#ifdef __cplusplus
}
#endif
