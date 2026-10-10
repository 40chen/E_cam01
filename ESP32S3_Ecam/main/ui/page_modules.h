/*
 * page_modules.h — 外接模块页
 *
 * 主菜单右侧"Modules"卡的落点。列出插在模块口（GPIO10/11）上的外设。
 */
#pragma once

#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

lv_obj_t *page_modules_create(lv_obj_t *parent);
void      page_modules_destroy(void);

#ifdef __cplusplus
}
#endif
