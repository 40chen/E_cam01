/*
 * app_audio.h — MP3 播放服务
 *
 * 负责：扫 /sdcard/MUSIC 生成播放列表、按 Helix 解码 MP3、经 I2S → ES8311 →
 * 板上功放出声，并把"放到哪儿了"报给界面。
 *
 * 和 app_camera 一样，**本模块不依赖 LVGL**：状态变化走回调，由界面层自己
 * 用 lv_async_call 转回 LVGL 上下文。回调是在解码任务里执行的，别在里面
 * 直接碰控件，也别在里面干慢活（那会把声音拖断）。
 *
 * 与相机的互斥（工程约定）：两者不能同时跑，它们抢内部 DMA RAM。
 * **落点在这一版挪了**：以前是"离开音乐页就停播"，现在音乐算"模式"——
 * 离开音乐页不停（频谱灯还要拿它当输入，见 page_music.c 的 maybe_stop_audio），
 * 改由**相机页**在进页时同步停播并释放音频硬件资源。U 盘模式本来就会停。
 *
 * 采样率：卡里的 MP3 采样率各不相同（44.1k / 48k / 22.05k / 16k…），
 * 播放时按文件自己的采样率切 I2S 时钟，见 bsp_audio_set_fs()。
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

/* 播放列表上限。再多也放不下屏幕，而且这个数组是静态分配的 */
#define APP_AUDIO_MAX_TRACKS 100
/* 文件名缓冲长度（含结尾 0）。超出会被截断——只用于显示和拼路径，
 * 拼出来的路径如果打不开，界面会看到明确的失败而不是错放别的歌 */
#define APP_AUDIO_NAME_LEN   96

typedef enum {
    APP_AUDIO_IDLE = 0,     /* 没在放（停止 / 放完了 / 出错后停下） */
    APP_AUDIO_PLAYING,
    APP_AUDIO_PAUSED,
    APP_AUDIO_ERROR,        /* 上一次播放失败，界面可以提示后再调 stop */
} app_audio_state_t;

/*
 * 状态回调。**在解码任务上下文里执行**，必须立刻返回。
 * state 变化时触发；同一状态内换歌（自动下一首）也会触发一次，
 * 界面可以据此更新标题/歌词。
 */
typedef void (*app_audio_event_cb_t)(app_audio_state_t state, int index, void *user);

/* 建命令队列与解码任务。可重复调用，重复调用只生效第一次。 */
esp_err_t app_audio_init(void);

/* 注册状态回调（传 NULL 注销）。界面 destroy 时要先注销再 stop。 */
void app_audio_set_event_cb(app_audio_event_cb_t cb, void *user);

/*
 * PCM 抽头：每解码出一帧，在送去 I2S **之前**原样回调一次。
 * 给 WS2812 频谱灯用（app_led2812），别的地方不需要。
 *
 * 格式：交织的 16 位立体声；frames 是**帧**数（样本数是 frames*2）。
 * rate_hz 是这一首自己的采样率——卡里的歌 44.1k/48k/22.05k/16k 都有，
 * 做频率分析必须知道当前是多少，不要假定 44.1k。
 *
 * **在解码任务上下文里执行，必须立刻返回，绝不能阻塞**——在这里卡一下，
 * 声音就跟着断一下。想干重活先拷进自己的缓冲，交给别的任务去做。
 */
typedef void (*app_audio_pcm_cb_t)(const int16_t *pcm, size_t frames,
                                   uint32_t rate_hz, void *user);
void app_audio_set_pcm_cb(app_audio_pcm_cb_t cb, void *user);

/*
 * 扫描 dir（传 NULL 用 APP_MUSIC_DIR）下的 .mp3（后缀大小写不敏感），
 * 按文件名升序排好。返回曲目数（0 = 空目录或 TF 卡不可用）。
 * 会重新开始播放列表，但不会打断正在播的那首。
 */
int app_audio_scan(const char *dir);

int         app_audio_track_count(void);
const char *app_audio_track_name(int index);

/* 播放第 index 首（越界会被夹到范围内；列表为空返回 ESP_ERR_INVALID_STATE） */
esp_err_t app_audio_play_index(int index);
esp_err_t app_audio_play_next(void);
esp_err_t app_audio_play_prev(void);

/* 空闲时是"开始播当前这首"；播放中暂停；暂停中继续 */
esp_err_t app_audio_toggle(void);
esp_err_t app_audio_pause(void);
esp_err_t app_audio_resume(void);

/* 停止播放、关功放、释放解码器。停完可以立刻再 play */
esp_err_t app_audio_stop(void);

/* 停止并等待音频任务完成清理、释放硬件资源；超时返回 ESP_ERR_TIMEOUT。 */
esp_err_t app_audio_stop_and_wait(uint32_t timeout_ms);

/* 跳到指定的播放位置（毫秒）。CBR 准确，VBR 是按字节比例估的近似值 */
esp_err_t app_audio_seek_ms(uint32_t ms);

/* 音量 0~100，走 ES8311 的 DAC 音量寄存器（切采样率后由 BSP 自动补回） */
esp_err_t app_audio_set_volume(int volume);
esp_err_t app_audio_get_volume(int *volume);

/* 下面三个可以在任意任务里读。都是 32 位对齐的单个变量，
 * 读到的值最多旧一帧（约 26ms），不会读到半个数字 */
app_audio_state_t app_audio_state(void);
int               app_audio_current_index(void);
uint32_t          app_audio_position_ms(void);
uint32_t          app_audio_duration_ms(void);
