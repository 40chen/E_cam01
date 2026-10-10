/*
 * wifi_prov.h — 凭据存储 + STA 连接
 *
 * 凭据是 Ecam 通过 UART 推过来的（`#wifi 1/2/3` 三行事务，见 link.c），
 * 存进 NVS，掉电还在——车不能每次上电都等主机推一遍。
 */
#pragma once

#include <stdbool.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* 建 netif/event/wifi，有存过的凭据就自动连。非阻塞 */
esp_err_t wifi_prov_start(void);

/*
 * 拿到 IP（或掉线时传空串）时回调一次。link.c 用它去发 `#ip`，
 * 这样 wifi 这层不用认识 UART。
 * **回调在自己的事件任务里执行**，别在里面干慢活。
 */
void wifi_prov_set_ip_cb(void (*cb)(const char *ip));

/*
 * 存下凭据并开始连。传空串表示清掉。
 * 返回 ESP_ERR_INVALID_ARG 表示太长了。
 */
esp_err_t wifi_prov_set(const char *ssid, const char *pass);

/* 当前 IP（没连上返回空串） */
const char *wifi_prov_ip(void);

bool wifi_prov_connected(void);

/* 上一次连接失败的原因（没失败过返回空串），给 UART 那侧报错用 */
const char *wifi_prov_last_error(void);

#ifdef __cplusplus
}
#endif
