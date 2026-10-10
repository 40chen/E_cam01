/*
 * web.h — 手机网页遥控
 *
 * C2 连上 WiFi 之后起一个 HTTP 服务，手机浏览器打开 `http://<C2 的 IP>/`
 * 就是一个摇杆页面。IP 从哪来：C2 通过 UART 报给 Ecam，Ecam 显示在履带车页
 * 上（C2 自己没有屏幕）。
 */
#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* 起 HTTP 服务。没连上 WiFi 也能起，只是没人能访问到 */
esp_err_t web_start(void);

#ifdef __cplusplus
}
#endif
