/*
 * ESP32C2_tank — 履带车模块（Ecam 外接模块之二）
 *
 * 这块板子挂在一辆双电机履带底盘上，通过 UART 听 Ecam 的指挥：
 *
 *   Ecam（主机，摇杆 + 屏幕）        C2（本工程，车上）
 *        UART1 TX ──────────────→ RX  运动指令 x<f> y<f>
 *        UART1 RX ←────────────── TX  身份 / IP / 凭据结果
 *
 * 三件事：
 *   1. **差速电机控制**（motion.c）——LEDC 两路，带 500ms 看门狗；
 *   2. **UART 协议**（link.c）——行式解析，不认识的行必须无害；
 *   3. **WiFi 凭据 + 网页遥控**（wifi_prov.c / web.c）——凭据由 Ecam 推过来
 *      存进 NVS，连上之后手机浏览器打开它的 IP 就能开这辆车。
 *
 * 启动顺序是有讲究的：
 *   motion  → link  → wifi  → web
 * 电机先停稳再听指令；链路早早开着（Ecam 一插上就会发 `#hi` 和运动指令）；
 * 网页最后起（它要等 IP，而 IP 归 wifi 管）。**任何一步失败都不中止**：
 * 车上没有屏幕，"起不来"和"跑起来了但没连上网"必须能从日志里分清。
 */
#include "esp_log.h"
#include "nvs_flash.h"

#include "link.h"
#include "motion.h"
#include "udp.h"
#include "web.h"
#include "wifi_prov.h"

static const char *TAG = "main";

void app_main(void)
{
    /* NVS：凭据要落盘，这是车上唯一要存的东西 */
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "NVS 需要重建（%s），旧凭据会丢", esp_err_to_name(err));
        nvs_flash_erase();
        err = nvs_flash_init();
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "NVS 起不来：%s（凭据存不住，但别的照跑）", esp_err_to_name(err));
    }

    /* 电机：**第一个初始化**。初始化里就把它停住，绝不让它带着上次的
     * duty 上电（虽然 LEDC 上电本来就是 0，但这里不靠"本来"） */
    err = motion_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "电机初始化失败：%s（这辆车动不了）", esp_err_to_name(err));
    }

    /* 链路：早开。Ecam 那边一选上模块就会发 `#hi` */
    err = link_start();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "UART 链路起不来：%s", esp_err_to_name(err));
    }

    /* WiFi：拿到 IP 就走回调通知链路去报 `#ip`（Ecam 显示在履带车页上） */
    wifi_prov_set_ip_cb(link_send_ip);
    err = wifi_prov_start();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "WiFi 起不来：%s", esp_err_to_name(err));
    }

    /* 无线驱动：收 Ecam 的摇杆包。它和网页遥控用的是同一个 motion_drive()，
     * 所以两边谁在开车都行，看门狗对两个来源一视同仁 */
    err = udp_start();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "UDP 起不来：%s（Ecam 那条无线驱动就没了）", esp_err_to_name(err));
    }

    /* 网页：即使还没连上网也先起服务，等 IP 一到就能访问 */
    err = web_start();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "HTTP 服务起不来：%s", esp_err_to_name(err));
    }

    ESP_LOGI(TAG, "=== 就绪：%s ===",
             wifi_prov_connected() ? wifi_prov_ip() : "等 WiFi 凭据（从 Ecam 推过来）");
}
