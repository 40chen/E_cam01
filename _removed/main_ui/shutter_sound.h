/*
 * shutter_sound.h — 快门声 PCM
 *
 * 定义在 shutter_sound.c 里，那个文件是 tools/gen_shutter_sound.py 从
 * assets/quick-camera-click.wav 生成的，不要手改。换音效就重新跑一次脚本。
 *
 * 数据是交错立体声 16bit PCM，放在 **flash** 里。I2S 的 DMA 读不了 flash，
 * 所以播放前必须先 heap_caps_malloc(..., MALLOC_CAP_DMA) 拷一份过去。
 */
#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    const uint8_t *pcm;     /* 交错立体声 16bit，flash 里 */
    uint32_t       bytes;
    uint32_t       rate;    /* 采样率 Hz。**必须等于 BSP_AUDIO_DEFAULT_RATE**：
                             * 播放时 bsp_audio_set_fs(rate,16,2) 只有和它相等才
                             * 是空操作；不等就要在运行期切采样率，而那条路径会
                             * 把 ES8311 的时钟分频器写坏（见 lckfb_bsp.c）。 */
    uint32_t       frames;  /* 每声道的采样数 */
} shutter_sound_t;

extern const shutter_sound_t shutter_sound;

#ifdef __cplusplus
}
#endif
