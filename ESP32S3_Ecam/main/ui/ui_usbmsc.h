/*
 * ui_usbmsc.h — U 盘模式的界面
 *
 * 和 ui_scan / ui_input 一样，这是**盖在页面上的浮层**，不是 ui.c 页面表里
 * 的一页：它从设置页的那一行点进来，关掉之后还回设置页。
 */
#pragma once

#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * 进入 U 盘模式并弹出模式页。已经开着的话什么都不做。
 * 进不去（没插卡、建卡失败……）就弹一条提示，不打开模式页。
 * 必须在 LVGL 上下文里调用（事件回调或 lv_async_call）。
 */
void ui_usbmsc_open(void);

#ifdef __cplusplus
}
#endif
