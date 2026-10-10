/*
 * app_wifi.h — WiFi STA 连接服务
 *
 * 负责 STA 模式连接、有限次自动重连、连接状态维护，以及拿到 IP 之后的
 * SNTP 对时（照片文件名依赖系统时间，见 app_camera.c）。
 *
 * 凭据由 app_settings 管理，本模块只负责连接，不碰 NVS。
 * 线程安全：app_wifi_connect() / status 查询可在任意任务调用；
 * 内部状态只在 WiFi/IP 事件回调里迁移（esp_event 单任务）。
 */
#pragma once

#include <stdbool.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    APP_WIFI_IDLE = 0,          /* 未配置 / 未请求连接 */
    APP_WIFI_CONNECTING,        /* 正在连接或重连 */
    APP_WIFI_CONNECTED,         /* 已拿到 IP */
    APP_WIFI_FAILED,            /* 认证失败或重试耗尽 */
} app_wifi_status_t;

/*---------------------------------------------------------------------------
 * 扫描
 *-------------------------------------------------------------------------*/
#define APP_WIFI_SCAN_MAX  20   /* 一次扫描最多保留多少个 AP */

typedef struct {
    char    ssid[33];
    int8_t  rssi;               /* dBm，越接近 0 越强 */
    bool    needs_pass;         /* 加密网络需要密码；开放网络直接连 */
} app_wifi_ap_t;

/*
 * 扫描结果回调。**在扫描任务的上下文里执行，不是 LVGL 任务**，
 * 所以回调里不能碰控件，要做 UI 得自己 lv_async_call。
 * count == 0 且 err == ESP_OK 表示扫到了但一个 AP 都没有。
 * aps 指向的数组只在回调期间有效，要留就得自己拷走。
 */
typedef void (*app_wifi_scan_cb_t)(const app_wifi_ap_t *aps, int count, esp_err_t err);

/* 初始化 netif / 事件循环 / WiFi 驱动并开机 STA。失败非致命 */
esp_err_t app_wifi_init(void);

/* 用给定凭据连接（非阻塞）。已连接时调用 = 断开换新凭据重连 */
esp_err_t app_wifi_connect(const char *ssid, const char *pass);

/* 断开并停止一切重连（"重置 WiFi"时用） */
esp_err_t app_wifi_disconnect(void);

/* 用上次的凭据再试一次（失败后由用户在设置页手动触发） */
esp_err_t app_wifi_retry(void);

/*
 * 发起一次扫描（非阻塞：内部起 worker 任务，扫完调 cb）。
 * 结果已按 RSSI 降序排好，同 SSID 的多个 BSSID 只留最强的一条。
 * 同一时刻只允许一次扫描，重复调用返回 ESP_ERR_INVALID_STATE。
 *
 * 不广播 SSID 的网络不会出现在结果里：驱动只能给出一个空 SSID，
 * 而 esp_wifi_set_config 必须要有 SSID 才能连，列出来也点不动。
 */
esp_err_t app_wifi_scan(app_wifi_scan_cb_t cb);

bool app_wifi_is_scanning(void);

app_wifi_status_t app_wifi_status(void);
bool app_wifi_is_connected(void);

/* 状态文案，如 "Connected 192.168.1.23" / "Connecting..." / "Auth failed"。
 * 返回指向静态缓冲的指针，下次调用会覆盖。 */
const char *app_wifi_status_str(void);

#ifdef __cplusplus
}
#endif
