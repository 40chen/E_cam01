/*
 * app_usbext.h — USB 副屏模式：把整机变成电脑的一块显示器
 *
 * 电脑（装了 Espressif 那个 Windows IDD 驱动）把桌面编码成 JPEG，通过**原生
 * USB 口**的 vendor 批量端点推过来；板子解出来，整屏画在 ST7789 上。
 *
 * 和 app_pcmon（"Status Display"）的区别，这两个名字很容易混：
 *
 *   app_pcmon  上位机 → UART0（CH340K 那个 COM 口），推的是 JSON 状态，
 *              显示在 LVGL 的副屏页上，LVGL 照常跑
 *   app_usbext 电脑 → 原生 USB 口，推的是画面，**整块屏幕都归它**，
 *              LVGL 在这期间是停的（见 ui_suspend）
 *
 * **串口不受影响**：日志和烧录走板载 CH340 → UART0，不经过 ESP32 的 USB PHY。
 * 被这个模式占用的是原生 USB 口。这一点比参考项目（ESP-SparkBot 只有一个 USB
 * 口，进了模式就得按住 BOOT 才能再烧录）好办得多。
 *
 * **退出只能靠板上的 BOOT 键**：进模式后屏幕上没有任何 UI（LVGL 停摆 + 面板被
 * 电脑的画面占满），触摸也一起停了。所以本模式进入前必须让用户知道怎么出来——
 * Work Mode 页上那行说明就是唯一的入口，别把它删了。
 *
 * **开关状态不存在任何地方，掉电即关**：和其它模式开关一样。这里还多一条理由：
 * 副屏要电脑那头一直在推才有意义，开机自动进这个模式的话，屏幕会停在一片
 * 没有画面的蓝色上，而唯一的出口是个用户根本不知道要按的键。
 */
#pragma once

#include <stdbool.h>

#include "esp_err.h"

/*
 * 进入副屏模式。会依次：
 *   停相机、音频、外接模块 → 分配缓冲 → 装 TinyUSB → 让出屏幕 → 起渲染任务
 *
 * **失败时不会把屏幕交出去**：所有可能失败的步骤都排在"让出屏幕"之前，
 * 所以失败时用户看到的还是正常的 Work Mode 页 + 一句 toast，而不是一块死屏。
 *
 * U 盘模式开着时返回 ESP_ERR_INVALID_STATE（两个模式抢同一个 USB 口）。
 */
esp_err_t app_usbext_enter(void);

/*
 * 退出副屏模式，把屏幕还给 LVGL。
 *
 * **只是置一个标志，善后由一个已经在跑的任务做**，所以本函数立刻返回，不会
 * 阻塞到你看见屏幕恢复。真正的清理（卸 TinyUSB、释放缓冲、ui_resume）在渲染
 * 任务那一侧完成。没开着时是空操作。
 *
 * 正常路径上不需要外部调用它——渲染任务自己会轮询 BOOT 键。
 */
void app_usbext_exit(void);

/* 模式是否开着（渲染任务还活着就是开着） */
bool app_usbext_active(void);

#ifdef __cplusplus
}
#endif
