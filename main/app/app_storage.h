/*
 * app_storage.h — TF 卡挂载与目录约定
 *
 * 板上所有用户数据都在 TF 卡上，固件本身不带存储分区：
 *   /sdcard/PHOTO  —— 拍照保存的 JPEG
 *   /sdcard/MUSIC  —— 待播放的 MP3
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#define APP_PHOTO_DIR  "/sdcard/PHOTO"
#define APP_MUSIC_DIR  "/sdcard/MUSIC"

/* 挂载 TF 卡并确保上述目录存在。失败不致命，界面会显示 TF 卡不可用。 */
esp_err_t app_storage_init(void);

/* 重新挂载（U 盘模式退出后需要），会先卸载再挂载 */
esp_err_t app_storage_remount(void);

/* 卸载（进入 U 盘模式前需要） */
esp_err_t app_storage_deinit(void);

bool app_storage_ready(void);

/* 剩余空间（字节），取不到时返回 0 */
uint64_t app_storage_free_bytes(void);

/* 遍历某个目录，回调返回 false 可提前结束。
 * name 为文件名（不含路径）。 */
typedef bool (*app_storage_iter_cb_t)(const char *name, void *user);
esp_err_t app_storage_list(const char *dir, const char *suffix,
                           app_storage_iter_cb_t cb, void *user);
