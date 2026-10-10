/*
 * app_usbmsc.h — U 盘模式（USB MSC）
 *
 * 把 TF 卡作为裸块设备交给电脑，让它在资源管理器里变成一个可读写盘符。
 *
 * 进模式期间**卡归电脑**：FATFS 已卸载，本机读不到任何文件（音乐页会显示
 * "No TF card"，相册页同理）。这是设计如此——同一张卡的 FAT 不能两边同时写。
 *
 * 串口**不受影响**：日志和烧录走的是板载 CH340 → UART0，不经过 ESP32 的
 * USB PHY；被这个模式占用的是**原生 USB 口**（它从 USB-Serial-JTAG 变成
 * 大容量存储设备）。
 */
#pragma once

#include <stdbool.h>

#include "esp_err.h"

/*
 * 进入 U 盘模式。会依次：停相机和播放 → 卸载 FATFS → 重新建卡（不挂文件系统）
 * → 建 MSC 存储 → 装 TinyUSB。
 *
 * **任何一步失败都会把设备恢复原样**（至少把 FATFS 挂回去），不会停在
 * "卡既没挂也没暴露"的中间态。失败原因用 esp_err_to_name() 显示即可。
 * 没有 TF 卡时返回 ESP_ERR_INVALID_STATE。
 */
esp_err_t app_usbmsc_enter(void);

/*
 * 退出 U 盘模式，重新挂回 FATFS，恢复正常设备。
 * **调用前应确保已在电脑上安全弹出该磁盘**，否则电脑那边可能还有没写完的数据。
 */
esp_err_t app_usbmsc_exit(void);

bool app_usbmsc_active(void);
