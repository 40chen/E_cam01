/*
 * app_settings.h — NVS 配置存储
 *
 * WiFi 与邮箱凭据的读写。所有值在 app_settings_init() 时一次性读进 RAM，
 * getter 只返回内存里的指针；setter 写 RAM 并落盘（nvs_set_str + commit）。
 * 空字符串表示"未设置"，set 空串等于擦除该键。
 *
 * 失败只记日志返回 esp_err_t，绝不中止启动（见 app_main.c 的容错原则）。
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* 各字段的最大字符数（不含结尾 NUL） */
#define APP_SETTINGS_SSID_MAX     32
#define APP_SETTINGS_PASS_MAX     64
#define APP_SETTINGS_HOST_MAX     63
#define APP_SETTINGS_PORT_MAX     5
#define APP_SETTINGS_ADDR_MAX     63

/* 启动时读入全部配置。NVS 不可用也能启动，只是读到空值 */
esp_err_t app_settings_init(void);

/* ---- getter：未设置时返回 "" ---- */
const char *app_settings_wifi_ssid(void);
const char *app_settings_wifi_pass(void);
const char *app_settings_smtp_host(void);
const char *app_settings_smtp_port(void);
const char *app_settings_mail_from(void);
const char *app_settings_mail_pass(void);
const char *app_settings_mail_to(void);

bool app_settings_has_wifi(void);   /* SSID 非空 */
bool app_settings_has_mail(void);   /* 发件地址非空 */

/* ---- setter：立即写 NVS 并 commit ---- */
esp_err_t app_settings_set_wifi_ssid(const char *val);
esp_err_t app_settings_set_wifi_pass(const char *val);
esp_err_t app_settings_set_smtp_host(const char *val);
esp_err_t app_settings_set_smtp_port(const char *val);
esp_err_t app_settings_set_mail_from(const char *val);
esp_err_t app_settings_set_mail_pass(const char *val);
esp_err_t app_settings_set_mail_to(const char *val);

/* ---- 重置：擦除一组配置的所有键 ---- */
esp_err_t app_settings_reset_wifi(void);
esp_err_t app_settings_reset_mail(void);

#ifdef __cplusplus
}
#endif
