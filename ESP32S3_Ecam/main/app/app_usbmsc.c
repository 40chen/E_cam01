/*
 * app_usbmsc.c — U 盘模式实现
 *
 * 为什么不照官方例程那样"开机建一次 MSC 存储、用 set_storage_mount_point()
 * 在应用和电脑之间切"：那样 MSC 存储层要**自己拥有 FATFS**（它以 MOUNT_APP
 * 挂着的时候，文件系统是它挂的），而本工程的 app_storage 一直是自己
 * esp_vfs_fat_sdmmc_mount 的。两套挂载并存会互相踩。所以这里**只在进模式的
 * 时候建 MSC 对象**，正常路径一行不动。
 *
 * 顺序是有讲究的，尤其是"先卸 FATFS 再建卡"这步——见下面 enter() 里的注释。
 */
#include "app_usbmsc.h"

#include "esp_log.h"

#include "tinyusb.h"
#include "tinyusb_default_config.h"
#include "tinyusb_msc.h"

#include "app_audio.h"
#include "app_camera.h"
#include "app_module.h"
#include "app_storage.h"
#include "lckfb_bsp.h"

static const char *TAG = "app_usbmsc";

#define MSC_EP_OUT 0x01
#define MSC_EP_IN  0x81

/* USB 描述符。产品名写清楚一点，电脑上"设备和打印机"里能认出来是谁 */
static const tusb_desc_device_t s_device_desc = {
    .bLength = sizeof(s_device_desc),
    .bDescriptorType = TUSB_DESC_DEVICE,
    .bcdUSB = 0x0200,
    .bDeviceClass = TUSB_CLASS_MISC,
    .bDeviceSubClass = MISC_SUBCLASS_COMMON,
    .bDeviceProtocol = MISC_PROTOCOL_IAD,
    .bMaxPacketSize0 = CFG_TUD_ENDPOINT0_SIZE,
    .idVendor = 0x303A,                 /* Espressif 的 VID（官方例程也用这个） */
    .idProduct = 0x4002,
    .bcdDevice = 0x100,
    .iManufacturer = 0x01,
    .iProduct = 0x02,
    .iSerialNumber = 0x03,
    .bNumConfigurations = 0x01,
};

static const uint8_t s_config_desc[] = {
    /* 配置号, 接口数, 字符串索引, 总长, 属性, 供电电流 mA */
    TUD_CONFIG_DESCRIPTOR(1, 1, 0, TUD_CONFIG_DESC_LEN + TUD_MSC_DESC_LEN,
                          TUSB_DESC_CONFIG_ATT_REMOTE_WAKEUP, 100),
    /* 接口号, 字符串索引, OUT 端点, IN 端点, 端点大小（S3 是全速，64） */
    TUD_MSC_DESCRIPTOR(0, 0, MSC_EP_OUT, MSC_EP_IN, 64),
};

static const char *s_string_desc[] = {
    (const char[]){ 0x09, 0x04 },       /* 0: 语言 = 英语(0x0409) */
    "ERDCX",                            /* 1: 厂商 */
    "ESP32S3_Ecam TF Card",             /* 2: 产品名 */
    "0001",                             /* 3: 序列号 */
    "TF Card",                          /* 4: MSC 接口 */
};

static tinyusb_msc_storage_handle_t s_storage;
static bool s_active;
static bool s_usb_installed;

bool app_usbmsc_active(void)
{
    return s_active;
}

esp_err_t app_usbmsc_enter(void)
{
    if (s_active) {
        return ESP_OK;
    }
    if (!app_storage_ready() || bsp_sdcard_get_card() == NULL) {
        ESP_LOGW(TAG, "没有可用的 TF 卡");
        return ESP_ERR_INVALID_STATE;
    }

    /* 1. 先停掉吃卡的东西。相机和音频本来就互斥，这里两个都停 */
    app_camera_stop();
    app_audio_stop();

    /*
     * 履带车也停。它并不吃 TF 卡，但**此时屏幕已经不在控制页上了**：
     * 用户在 U 盘模式里翻文件，而车还挂着一条链路在 20Hz 发指令。
     * 车身看门狗 500ms 会停（没有新指令时），可"设备已经换了个形态、
     * 车却还在听"这件事本身就不该存在——安全相关的东西宁可多停一次。
     *
     * 走 deselect 而不是 app_tank_disable()：后者只拆链路、不清选择，
     * Modules 页上会留一个"Running"的假状态。deselect 两件事一起做。
     * 氛围灯不停：它和卡、和这条路都不冲突，关掉只是白白打断用户。
     */
    app_module_deselect();

    /*
     * 2. 卸 FATFS —— 必须在把设备交给电脑**之前**做完。
     *    两边同时写同一张卡的 FAT，坏文件系统是迟早的事。
     */
    esp_err_t err = app_storage_deinit();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "卸载 FATFS 失败: %s", esp_err_to_name(err));
        return err;                     /* 卡还归本机，状态是好的 */
    }

    /*
     * 3. 重新建卡。卸载会把 SDMMC 主机 deinit、卡结构体 free 掉
     *    （IDF 的 unmount_card_core 里 call_host_deinit + free），
     *    所以**没有**现成的卡可以交给 TinyUSB，只能重建一张。
     */
    err = bsp_sdcard_card_init();
    if (err != ESP_OK) {
        goto restore_fs;
    }

    {
        tinyusb_msc_storage_config_t cfg = {
            /* 只给电脑，不给应用：这样 MSC 层不会去挂 FATFS，也就不会和我们抢 */
            .mount_point = TINYUSB_MSC_STORAGE_MOUNT_USB,
            .fat_fs = {
                .base_path = NULL,
                .config.max_files = 5,
                .format_flags = 0,
            },
        };
        cfg.medium.card = bsp_sdcard_get_card();
        err = tinyusb_msc_new_storage_sdmmc(&cfg, &s_storage);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "建 MSC 存储失败: %s", esp_err_to_name(err));
            goto free_card;
        }
    }

    {
        tinyusb_config_t tusb_cfg = TINYUSB_DEFAULT_CONFIG();
        tusb_cfg.descriptor.device = &s_device_desc;
        tusb_cfg.descriptor.full_speed_config = s_config_desc;
        tusb_cfg.descriptor.string = s_string_desc;
        tusb_cfg.descriptor.string_count = sizeof(s_string_desc) / sizeof(s_string_desc[0]);
        err = tinyusb_driver_install(&tusb_cfg);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "装 TinyUSB 失败: %s", esp_err_to_name(err));
            goto del_storage;
        }
    }

    s_usb_installed = true;
    s_active = true;
    ESP_LOGI(TAG, "U 盘模式已开启（卡已交给电脑，本机读不到文件了）");
    return ESP_OK;

del_storage:
    tinyusb_msc_delete_storage(s_storage);
    s_storage = NULL;
free_card:
    bsp_sdcard_card_deinit();
restore_fs:
    /* 走到这儿说明没能进模式，**必须把文件系统挂回去**，否则音乐、相册、
     * 设置里的剩余空间会一起失效，而用户只看到"进 U 盘模式失败" */
    if (app_storage_remount() != ESP_OK) {
        ESP_LOGE(TAG, "恢复 FATFS 失败——TF 卡暂时不可用");
    }
    return err;
}

esp_err_t app_usbmsc_exit(void)
{
    if (!s_active) {
        return ESP_OK;
    }

    /* 顺序照官方例程：先删存储（它会把该刷的刷下去），再卸驱动 */
    if (s_storage != NULL) {
        tinyusb_msc_delete_storage(s_storage);
        s_storage = NULL;
    }
    if (s_usb_installed) {
        tinyusb_driver_uninstall();
        s_usb_installed = false;
    }

    bsp_sdcard_card_deinit();

    esp_err_t err = app_storage_remount();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "重新挂载 TF 卡失败: %s", esp_err_to_name(err));
    }

    s_active = false;
    ESP_LOGI(TAG, "U 盘模式已关闭，TF 卡还给本机");
    return err;
}
