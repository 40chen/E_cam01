/*
 * app_lyrics.h — .lrc 歌词
 *
 * 和 MP3 同名的 .lrc 放在同一个目录里（/sdcard/MUSIC/歌名.lrc）。
 * 支持 UTF-8（带不带 BOM 都行）和 GB2312/GBK——网上下载的中文歌词
 * 后者居多，见 app_lyrics.c 里的编码识别。
 *
 * 本模块不依赖 LVGL，也不保留任何 LVGL 对象：界面每次拿"第几行"自己去刷。
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

/* 行数上限。一首歌的歌词一般不到 100 行，400 已经留足余量 */
#define APP_LYRICS_MAX_LINES 400

/*
 * 找 mp3_path 对应的 .lrc 并解析。
 *   ESP_OK            解析出至少一行带时间戳的歌词
 *   ESP_ERR_NOT_FOUND 没有同名 .lrc（界面据此显示黑胶）
 *   其它              读文件失败或内存不足
 * 可重复调用：会先把上一次的结果释放掉。
 */
esp_err_t app_lyrics_load(const char *mp3_path);

void app_lyrics_free(void);

/* 有歌词可用（load 成功之后为 true） */
bool app_lyrics_ready(void);
int  app_lyrics_count(void);

/* 第 index 行的文本；越界返回空串 */
const char *app_lyrics_line(int index);

/*
 * ms 时刻该高亮哪一行。返回 -1 表示还没到第一行的时间
 * （前奏阶段界面可以把所有行都按未选中显示）。
 * 时间戳必须是递增的——load 里已经排过序。
 */
int app_lyrics_index_at(uint32_t ms);

/* [ti:]/[ar:] 标签，没有时返回空串 */
const char *app_lyrics_title(void);
const char *app_lyrics_artist(void);
