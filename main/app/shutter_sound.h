/*
 * shutter_sound.h — 快门声 PCM
 *
 * 数据在 shutter_sound.c 里，那个文件由 tools/gen_shutter_sound.py 生成，
 * **不要手改**。换素材就重新跑一次脚本。
 *
 * 格式是交错立体声 16bit PCM，放在 **flash** 里。I2S 的 DMA 读不了 flash，
 * 所以播放时要用可 DMA 的内存；app_sound 那边是分块拷进一块 4KB 的缓冲里
 * 边拷边喂，不是整段复制——相机 + WiFi + LVGL 都跑着的时候内部 DMA 内存
 * 很紧（实测一次要 96KB 会直接失败）。
 *
 * **这里是一张表而不是一个数组**：2026-09-22 那段"一次编三个候选、在板子上
 * 点着挑"的过程已经结束（定稿 Raw，见脚本里的 VARIANTS），但当时为此写的
 * 表结构留着——以后要加音色，脚本加一行、这里什么都不用改。
 */
#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    const char    *name;    /* 调试用 */
    const uint8_t *pcm;     /* 交错立体声 16bit，flash 里 */
    uint32_t       bytes;
    uint32_t       rate;    /* 采样率 Hz，等于 BSP_AUDIO_DEFAULT_RATE */
    uint32_t       frames;  /* 每声道的采样数 */
} shutter_sound_t;

#define SHUTTER_SOUND_COUNT 1

extern const shutter_sound_t shutter_sound_variants[SHUTTER_SOUND_COUNT];

#ifdef __cplusplus
}
#endif
