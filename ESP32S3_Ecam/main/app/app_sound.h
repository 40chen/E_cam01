/*
 * app_sound.h — 一次性音效（快门声）
 *
 * 和 app_audio 是两回事：那边是长播放，有解码任务、播放列表、进度和命令
 * 队列；这边就一句"放一小段 PCM 然后收摊"。刻意不复用 app_audio——它的
 * 解码器和队列对一个 250ms 的"咔哒"全是负担。
 *
 * 工程里那条"相机和音乐互斥"的约定**在这里不适用**：快门声恰恰要在相机
 * 正跑着的时候响。所以唯一的资源约束是"别多占内部 DMA 内存"，见 .c 里的说明。
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * 播一段 PCM（交错立体声 16bit）。**阻塞**：写完还在等 DMA 排空，全部放完
 * 才返回，所以必须在自己的任务里调，不能在 LVGL 任务里调。
 * rate 必须是 bsp_audio_set_fs() 支持的档位（见 lckfb_bsp.h）。
 */
esp_err_t app_sound_play_pcm(const uint8_t *pcm, uint32_t bytes, uint32_t rate);

/*---------------------------------------------------------------------------
 * 快门声
 *
 * "一次编三个候选、在板子上点着挑"那个阶段已经结束（定稿 Raw），现在只剩
 * 一个音色加一个开关。
 *
 * **开关只存在内存里，掉电回到开**：它跟音乐页那个音量一样，目前都不是
 * 持久化设置。要落盘得给 app_settings 加键，还没做。
 *-------------------------------------------------------------------------*/

/* 设置页那个开关。默认开 */
void app_sound_set_shutter_enabled(bool on);
bool app_sound_shutter_enabled(void);

/* 播一声快门声。开关关着时什么都不做、直接返回 ESP_OK。阻塞，同上 */
esp_err_t app_sound_play_shutter(void);

/*
 * 播一声快门声，**无视开关**。
 *
 * 给设置页那个音量滑条用的：调音量不出声等于没调，而且它顺带顶替了原来
 * Test Speaker 的位置——"这条音频通路还通不通"现在靠拖一下音量滑条来验。
 */
esp_err_t app_sound_play_shutter_preview(void);

#ifdef __cplusplus
}
#endif
