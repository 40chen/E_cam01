/*
 * udp.h — 无线驱动链路（Ecam → 车）
 *
 * Ecam 往这个端口单播 20Hz 的 `x<f> y<f>`，和 UART 上那套是**同一套文本
 * 协议**，所以两边共用一个解析器（link_handle_line）。
 *
 * 这条链路的意义：**插着线只是为了把 WiFi 凭据交过来，交完就能拔掉**，
 * 之后车跑开了照样开。
 *
 * 收包的源地址就是 Ecam，车用它回 1Hz 的 `#ip` 心跳（Ecam 靠它显示"连着"）。
 */
#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* 两边必须一致：Ecam 那边的 APP_TANK_UDP_PORT */
#define UDP_PORT   3333

/* 建 socket + 起收包任务。没连上 WiFi 也能起，只是收不到东西 */
esp_err_t udp_start(void);

#ifdef __cplusplus
}
#endif
