/*
 * ESP32S3_Ecam — 立创·实战派 ESP32-S3 相机 + 音乐播放器
 *
 * 功能：取景预览、拍照存 TF 卡、照片浏览、MP3 播放、WiFi 邮件发送、
 *       Type-C U 盘模式、触摸屏配置 WiFi/邮箱。
 *
 * 结构：
 *   components/lckfb_bsp/  板级支持（I2C / LCD / 背光 / 触摸 / TF / 相机 / 音频）
 *   main/app/              业务服务（存储、相机、音频、网络、邮件、U 盘）
 *   main/ui/               LVGL 界面（主菜单 + 各全屏子页）
 *
 * 启动容错原则（重要）：
 *   任何单个外设初始化失败都只记录日志并继续，绝不用 ESP_ERROR_CHECK 中止。
 *   中止会让芯片反复复位，从外面看就是「背光不亮、屏幕全黑」一个现象，
 *   真正的故障点反而被掩盖。现在无论哪一步失败，屏幕和背光都会亮起来，
 *   串口也会明确指出是哪一步。
 */
#include <stdlib.h>
#include <time.h>

#include "esp_err.h"
#include "esp_log.h"
#include "nvs_flash.h"

#include "app_settings.h"
#include "app_storage.h"
#include "app_wifi.h"
#include "lckfb_bsp.h"
#include "ui.h"

static const char *TAG = "app";

static bool nvs_init(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        /* 换固件后旧数据会让 nvs_flash_init 失败，这是烧录新固件后
         * 最常见的一类「什么都跑不起来」的原因。擦掉重建即可，
         * 代价是之前存的 WiFi / 邮箱配置会丢失。 */
        ESP_LOGW(TAG, "NVS unusable (%s), erasing and retrying", esp_err_to_name(err));
        err = nvs_flash_erase();
        if (err == ESP_OK) {
            err = nvs_flash_init();
        }
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "NVS init failed: %s (settings will not persist)", esp_err_to_name(err));
        return false;
    }
    return true;
}

void app_main(void)
{
    /* 时区设成北京时间。app_camera.c 用 localtime_r 生成照片文件名，
     * 不设时区的话 SNTP 对完时文件名也是 UTC，差 8 小时。 */
    setenv("TZ", "CST-8", 1);
    tzset();

    /*
     * 第一条硬件动作是配置背光。它只用到 LEDC，不依赖 I2C、SPI 或任何
     * 其它外设，所以“屏幕亮不亮”能直接回答一个关键问题：固件到底跑起来了
     * 没有。如果连背光都不亮，问题就在烧录或启动，而不在这份代码里。
     *
     * 注意这里只是**配置**，真正点亮在 bsp_display_init() 里：面板复位后
     * GRAM 是上电随机值，必须在刷黑 GRAM、打开显示之后才点灯，否则会闪
     * 一下花屏。面板若初始化失败，bsp_display_init() 会强制点亮背光，
     * 所以上面那个判断依据不受影响。
     */
    esp_err_t berr = bsp_backlight_init();
    ESP_LOGI(TAG, "=== ESP32S3_Ecam boot === backlight=%s",
             berr == ESP_OK ? "configured" : esp_err_to_name(berr));

    bool ok_nvs = nvs_init();

    /* ---- I2C：PCA9557、QMI8658、触摸、ES8311、摄像头 SCCB 共用 ---- */
    esp_err_t err = bsp_i2c_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "I2C init failed: %s (LCD CS / camera / touch all depend on it)",
                 esp_err_to_name(err));
    }

    /* ---- PCA9557：屏幕片选、功放使能、摄像头电源 ---- */
    err = bsp_pca9557_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "PCA9557 init failed: %s (LCD CS cannot be asserted -> screen stays blank)",
                 esp_err_to_name(err));
    }

    /* ---- 屏幕 + 背光：尽早点亮，作为后续一切判断的基准 ---- */
    err = bsp_display_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "display init failed: %s", esp_err_to_name(err));
    }

    /* ---- LVGL 界面：先亮出 ERDCX 启动页 ---- */
    err = ui_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "UI init failed: %s", esp_err_to_name(err));
        return;                 /* LVGL 起不来就没有后续可做了 */
    }

    /* ---- TF 卡：放在启动页之后，挂卡这几百毫秒正好藏在启动页停留的
     *      时间里；主菜单出来时状态栏的 TF 卡图标就是准的 ---- */
    bool ok_sd = (app_storage_init() == ESP_OK);

    /* ---- 配置与 WiFi：全部非致命 ---- */
    esp_err_t serr = app_settings_init();
    if (serr != ESP_OK) {
        ESP_LOGW(TAG, "settings init failed: %s (fields will read as empty)",
                 esp_err_to_name(serr));
    }
    serr = app_wifi_init();
    if (serr != ESP_OK) {
        ESP_LOGE(TAG, "wifi init failed: %s", esp_err_to_name(serr));
    } else if (app_settings_has_wifi()) {
        /* 有存过的凭据就自动连，连不上设置页里能看原因 */
        app_wifi_connect(app_settings_wifi_ssid(), app_settings_wifi_pass());
    }

    ESP_LOGI(TAG, "=== boot summary: nvs=%s  sd=%s  ui=ok  wifi=%s ===",
             ok_nvs ? "ok" : "FAIL", ok_sd ? "ok" : "FAIL",
             app_settings_has_wifi() ? "auto-connect" : "unconfigured");
    ESP_LOGI(TAG, "main menu: Camera / Music / Files / Settings");
}
