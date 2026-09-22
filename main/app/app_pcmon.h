/*
 * app_pcmon.h — 副屏：收上位机推过来的系统状态
 *
 * 上位机（assets/Ecam_display/Ecam_Status_Display.py）每秒往串口写一行 JSON，
 * 板子从 UART0 的 RX 收下来，解析出主机名 / CPU / 内存 / GPU / 网络 / 电池
 * 和 API 用量（tokens / 已消费 / 余额），供副屏页显示。
 *
 * **为什么是 UART0**：板子上那颗 CH334F HUB 把 Type-C 分给了 CH340K，CH340K
 * 接的正是 UART0（TX/RX = GPIO43/44），电脑上看到的那个 COM 口就是它。
 * 收 RX 和看日志不冲突——日志只走 TX，而且走的是 vfs_uart 的老路径
 * （见 .c 里的说明），装不装 UART 驱动都轮不到它。
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* 超过这个时间没收到新帧就算断线。上位机是 1 秒一帧，3 秒足够宽容 */
#define APP_PCMON_STALE_MS 3000

/* 主机名缓冲长度（含结尾 0） */
#define APP_PCMON_HOST_LEN 32

/*
 * 一份快照。**是纯数据、不含指针**，界面拿到手就是完整的一帧，
 * 不会读到"正在更新"的中间状态。
 */
typedef struct {
    bool     valid;         /* 收到过至少一帧 */
    uint32_t age_ms;        /* 距最后一帧多久；valid 为 false 时无意义 */

    char     host[APP_PCMON_HOST_LEN];
    float    cpu;           /* % */
    float    ram;           /* % */
    int      gpu;           /* %，-1 = 上位机没有这块数据（没有独显） */
    float    net_up;        /* KB/s */
    float    net_down;      /* KB/s */
    /* 上位机还会发 battery，这里**刻意不解析**：副屏上那行按需求删掉了，
     * 台式机上它一直是 -1，留着就是个永远用不上的字段。哪天要显示再加回来 */

    long long tokens;
    float     cost;         /* 元 */
    float     balance;      /* 元 */
} pcmon_state_t;

/*
 * 开始接收。装 UART 驱动 + 起接收任务，可重复调用（已经开着就返回 ESP_OK）。
 * 失败返回错误，界面提示后什么都不用回滚——失败时不会停在半开状态。
 */
esp_err_t app_pcmon_start(void);

/* 停止接收并释放 UART 驱动。没开着时是空操作 */
void app_pcmon_stop(void);

bool app_pcmon_is_running(void);

/*
 * 取一份快照。返回 false = 一帧都还没收到过（界面显示"等待上位机"）。
 * age_ms 每帧重新算，界面据此判断是不是断线了（见 APP_PCMON_STALE_MS）。
 */
bool app_pcmon_get(pcmon_state_t *out);

#ifdef __cplusplus
}
#endif
