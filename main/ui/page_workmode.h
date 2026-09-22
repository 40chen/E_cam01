/*
 * page_workmode.h — 工作模式页
 *
 * 主菜单右侧"Work Mode"卡的落点。页面本体由 ui.c 管（page_*_create /
 * page_*_destroy 那一对），所以这里只有那两个声明。
 *
 * 页面上的每一项都会改变设备的形态（把 TF 卡交给电脑、开始收上位机的状态、
 * ...），和设置页那种"填一次就不动"的凭据分开放。
 */
#pragma once

#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

lv_obj_t *page_workmode_create(lv_obj_t *parent);
void      page_workmode_destroy(void);

#ifdef __cplusplus
}
#endif
