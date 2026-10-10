/* page_mod_led.h — 模块控制页：WS2812 氛围灯 */
#pragma once

#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

lv_obj_t *page_mod_led_create(lv_obj_t *parent);
void      page_mod_led_destroy(void);

#ifdef __cplusplus
}
#endif
