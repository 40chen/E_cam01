/*
 * app_led2812.h — WS2812 氛围灯模块（外接模块之一）
 *
 * 驱动一块 8x8（64 颗）WS2812 灯板，DIN 接在模块口的 GPIO10 上（见 app_module.h）。
 * 两个效果**互斥**，同一时刻只有一个在跑：
 *
 *   MANUAL    手动氛围灯：整块板一个颜色，色轮选色 + 亮度滑条
 *   SPECTRUM  音乐频谱灯：8 列柱状，一列一个对数频段，高的一列是高频
 *
 * **频谱为什么不用麦克风**：板和喇叭在同一台设备上，放歌时麦克风听到的就是
 * 这把喇叭——音量和频率响应都是二手的，还容易啸叫。而解码后的 PCM 就在手边
 * （app_audio 的 write_pcm 之前），顺带省掉 ES7210 的初始化和一路 I2S RX。
 * 代价是**只有本机在放歌时才有反应**；音乐一停 PCM 就断，柱子自己衰减到全黑，
 * 不需要任何额外代码去处理这件事。
 *
 * 效果跑在自己的任务里（不是 lv_timer）：一次 refresh 是 ~1.9ms 的阻塞发送，
 * 塞进 LVGL 任务里每帧都会卡触摸和渲染。
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* 灯板规格。8x8 是硬件决定的，频段数 = 列数（别往 8 列里塞 16 段） */
#define APP_LED_COUNT   64
#define APP_LED_COLS    8
#define APP_LED_ROWS    8

typedef enum {
    APP_LED_EFFECT_MANUAL = 0,      /* 手动氛围灯 */
    APP_LED_EFFECT_SPECTRUM,        /* 音乐频谱灯 */
} app_led_effect_t;

/*
 * 启动灯板：建 RMT 通道 + 起效果任务。
 * **要求已经 app_module_select(APP_MODULE_LED2812)**，否则返回 ESP_ERR_INVALID_STATE
 * ——"一次只跑一个模块"这条规则落在服务层，不是靠界面自觉。
 * 已经开着时是空操作。
 */
esp_err_t app_led2812_enable(void);

/*
 * 停灯板：停任务、拆 RMT 通道、**把 GPIO10/11 还给空闲态**。
 * 最后那步不能省：RMT 驱动不会自己还脚，不还的话 GPIO10 就停在一根被驱动的
 * 推挽输出上——下次插上的是别的模块，那根线就是被硬顶着。
 */
void app_led2812_disable(void);

bool app_led2812_is_active(void);

/*
 * 频谱效果是不是正拿音乐当输入。给音乐页用：**音乐是"模式"不是"页面"**，
 * 离开音乐页默认要停播，唯独频谱灯正需要它的时候不能停。
 * 没有这个模块、或者不在频谱模式，都返回 false（音乐页照旧停播）。
 */
bool app_led2812_needs_audio(void);

/* 两个效果互斥，直接切 */
esp_err_t app_led2812_set_effect(app_led_effect_t effect);
app_led_effect_t app_led2812_effect(void);

/* 手动氛围灯的颜色（h/s 都是 0~255，h 是色轮角度）与全局亮度（0~100） */
esp_err_t app_led2812_set_hsv(uint8_t h, uint8_t s);
esp_err_t app_led2812_set_brightness(uint8_t percent);
uint8_t   app_led2812_brightness(void);

/*
 * 取一份频谱柱高快照（每个元素 0~APP_LED_ROWS）。界面画柱状条用。
 * 读的是最近一帧的快照，最坏旧一帧——和 app_audio_position_ms() 一个路子，
 * 界面永远不碰硬件。
 */
void app_led2812_get_levels(uint8_t *out, int max_bands);

#ifdef __cplusplus
}
#endif
