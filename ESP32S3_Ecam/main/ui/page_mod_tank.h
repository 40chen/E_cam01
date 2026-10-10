/* page_mod_tank.h — 模块控制页：履带车 */
#pragma once

#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

lv_obj_t *page_mod_tank_create(lv_obj_t *parent);
void      page_mod_tank_destroy(void);

#ifdef __cplusplus
}
#endif
