/*
 * page_pcmon.h — 副屏页（上位机状态）
 *
 * 显示上位机推过来的系统状态和 API 用量。数据来源是 app_pcmon，本页只负责
 * 显示——**不做任何解析**（JSON 那套东西留在 app_pcmon 里）。
 *
 * 开关在工作模式页（Work Mode → Status Display）。
 */
#pragma once

#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

lv_obj_t *page_pcmon_create(lv_obj_t *parent);
void      page_pcmon_destroy(void);

#ifdef __cplusplus
}
#endif
