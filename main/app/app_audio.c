/*
 * app_audio.c — MP3 播放服务实现
 *
 * 结构：一个解码任务 + 一个命令队列。
 *   - 播放中，解码循环每解一帧都顺手看一眼命令队列（非阻塞），所以暂停、
 *     停止、切歌最迟在一帧之内（约 26ms）生效，不用等整首歌放完。
 *   - 空闲时任务阻塞在队列上等命令。
 *   - 界面线程只往队列里塞命令，不直接碰播放状态，也就不存在竞态。
 *
 * 解码用 Helix 定点解码器（mp3dec.h），和立创官方例程 11-mp3_player 的内核
 * 同一个。**没有用 chmorgan/esp-audio-player 那一层**：它不对外给播放位置，
 * 而进度条和歌词同步都要位置；自己写这一圈解码循环反而更短。
 *
 * 采样率按文件切（bsp_audio_set_fs），因为卡里的 MP3 44.1k / 48k / 22.05k
 * 都有；ES8311 那几个时钟寄存器由 BSP 在每次 open 之后回读校验。
 */
#include "app_audio.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"

#include "mp3dec.h"

#include "app_storage.h"
#include "lckfb_bsp.h"

static const char *TAG = "app_audio";

#define AUDIO_TASK_STACK 6144
#define AUDIO_TASK_PRIO  4
/* 和相机任务同在 core 1：两者互斥，放一起能让 core 0 上的 LVGL 不被打扰 */
#define AUDIO_TASK_CORE  1

/*
 * 默认音量。0dB 大约在 93 附近（见 lckfb_bsp.h 的音量注释），所以这一档约
 * -21dB，留了足够余量。
 *
 * 为什么压这么低：2026-09-20 实测发现**音量大了小喇叭会失真**（"滋滋"的
 * 杂音），把音量调小就完全干净。当时已经用供给统计排除了"解码跟不上"——
 * 每 5 秒一行的日志显示音频产出和实际时间一比一、I2S 写入稳定阻塞 19.9ms/帧
 * （一帧 26.1ms），DMA 一直是喂饱的。所以那是**电平过载**（喇叭/功放），
 * 不是解码，也不是采样率。
 *
 * 这条和板载喇叭 3-5kHz 以上滚降是同一类硬件上限：**调高音量能变响，但超过
 * 某个点就开始失真，代码解决不了**。用户在界面上随时可以调更大。
 */
#define APP_AUDIO_DEFAULT_VOLUME 50

/* 解码输入缓冲。MAINBUF_SIZE 是 helix 建议的最小值（1940），取 3 倍是为了
 * 一次读盘能覆盖好几帧，少几次 SD 卡往返 */
#define MP3_BUF_BYTES (MAINBUF_SIZE * 3)

/* 缓冲尾部的零填充区。helix 解帧头和边信息时**不检查剩余长度**就直接往后读
 * 几个字节，读指针贴到缓冲末尾时它会读过界；给一块自己的填充区，越界读落在
 * 里面（读到 0 会被当成非同步字，安全），而不是碰到堆上相邻的分配。 */
#define MP3_BUF_PAD   64

/* 一帧最多 1152 帧 × 2 声道 = 2304 个采样；单声道转立体声要原地扩一倍，
 * 所以按 2 倍留量 */
#define PCM_BUF_BYTES (2 * MAX_NCHAN * MAX_NGRAN * MAX_NSAMP * (int)sizeof(int16_t))

/*---------------------------------------------------------------------------
 * 命令队列
 *-------------------------------------------------------------------------*/
typedef enum {
    CMD_PLAY = 0,   /* value = 曲目序号（播放中就切歌） */
    CMD_PAUSE,
    CMD_RESUME,
    CMD_TOGGLE,
    CMD_STOP,
    CMD_SEEK,       /* value = 目标毫秒 */
} audio_cmd_op_t;

typedef struct {
    audio_cmd_op_t op;
    int32_t        value;
    uint32_t       stop_id;
} audio_cmd_t;

/*---------------------------------------------------------------------------
 * 状态。s_index / s_state / s_pos_ms / s_duration_ms 会被界面线程读，
 * 都是 32 位对齐的单个变量，读到的最多旧一帧，不会读到半个数字。
 *-------------------------------------------------------------------------*/
static QueueHandle_t        s_cmd_q;
static TaskHandle_t         s_task;
static SemaphoreHandle_t    s_stop_mutex;
static app_audio_event_cb_t s_event_cb;
static void                *s_event_user;
static app_audio_pcm_cb_t   s_pcm_cb;       /* 频谱灯的抽头，见 app_audio.h */
static void                *s_pcm_user;

static char s_names[APP_AUDIO_MAX_TRACKS][APP_AUDIO_NAME_LEN];
static int  s_track_count;

static volatile int               s_index = -1;
static volatile app_audio_state_t s_state = APP_AUDIO_IDLE;
static volatile uint32_t          s_pos_ms;
static volatile uint32_t          s_duration_ms;
static int                        s_volume = APP_AUDIO_DEFAULT_VOLUME;
static uint32_t                   s_next_stop_id;
static volatile uint32_t          s_stop_ack_id;
static volatile esp_err_t         s_stop_result = ESP_OK;
static uint32_t                   s_playback_stop_id;

/* 把 s_volume（0~100）映射到 BSP 的 0~93 写下去，定义在文件后半 */
static void apply_volume(void);

static void notify_state(void)
{
    if (s_event_cb != NULL) {
        s_event_cb(s_state, s_index, s_event_user);
    }
}

static void set_state(app_audio_state_t st)
{
    s_state = st;
    notify_state();
}

static esp_err_t send_cmd(audio_cmd_op_t op, int32_t value)
{
    if (s_cmd_q == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    const audio_cmd_t cmd = { .op = op, .value = value, .stop_id = 0 };
    /* 队列满了说明解码任务卡住了，这种时候丢命令比阻塞界面线程好：
     * 界面卡住会连触摸都没反应，丢一条命令最多是"按了没生效" */
    return (xQueueSend(s_cmd_q, &cmd, 0) == pdTRUE) ? ESP_OK : ESP_ERR_TIMEOUT;
}

/*---------------------------------------------------------------------------
 * 播放列表
 *-------------------------------------------------------------------------*/
typedef struct {
    int count;
} scan_ctx_t;

static bool scan_cb(const char *name, void *user)
{
    scan_ctx_t *ctx = (scan_ctx_t *)user;
    if (ctx->count >= APP_AUDIO_MAX_TRACKS) {
        return false;                       /* 满了，提前结束遍历 */
    }
    strlcpy(s_names[ctx->count], name, APP_AUDIO_NAME_LEN);
    ctx->count++;
    return true;
}

int app_audio_scan(const char *dir)
{
    scan_ctx_t ctx = { .count = 0 };

    if (!app_storage_ready()) {
        s_track_count = 0;
        ESP_LOGW(TAG, "TF 卡不可用，播放列表清空");
        return 0;
    }

    const char *path = (dir != NULL) ? dir : APP_MUSIC_DIR;
    esp_err_t err = app_storage_list(path, ".mp3", scan_cb, &ctx);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "扫描 %s 失败: %s", path, esp_err_to_name(err));
        ctx.count = 0;
    }

    /* 文件名升序。卡里的曲子多半带 "01_" 这样的序号前缀，按名字排就是专辑顺序 */
    qsort(s_names, ctx.count, APP_AUDIO_NAME_LEN,
          (int (*)(const void *, const void *))strcasecmp);

    s_track_count = ctx.count;
    ESP_LOGI(TAG, "播放列表：%s 下 %d 首", path, s_track_count);
    return s_track_count;
}

int app_audio_track_count(void)
{
    return s_track_count;
}

const char *app_audio_track_name(int index)
{
    if (index < 0 || index >= s_track_count) {
        return "";
    }
    return s_names[index];
}

/*---------------------------------------------------------------------------
 * 解码与播放
 *-------------------------------------------------------------------------*/
static HMP3Decoder s_dec;
static uint8_t    *s_in_buf;
static int16_t    *s_pcm;

/* 当前文件的解析结果，seek 和进度条要用 */
static uint32_t s_rate;
static uint32_t s_file_size;
static uint32_t s_audio_start;      /* 音频数据起始偏移（跳过 ID3v2） */
static uint64_t s_frames_done;      /* 已解码的帧数（每声道），算播放位置用 */

/*
 * 重建解码器。seek 之后必须重建：Helix 内部有比特池和上一帧的残留状态，
 * 直接换个位置喂数据，头一两帧会出错。
 */
static esp_err_t decoder_reset(void)
{
    if (s_dec != NULL) {
        MP3FreeDecoder(s_dec);
    }
    s_dec = MP3InitDecoder();
    return (s_dec != NULL) ? ESP_OK : ESP_ERR_NO_MEM;
}

/*
 * ID3v2 标签长度（含 10 字节头）。长度字段是 syncsafe 编码——每字节只用低
 * 7 位，所以不能直接当大端 32 位读。返回 0 表示没有标签。
 * 调用后文件位置会移动，调用方自己 seek 回去。
 */
static uint32_t id3v2_bytes(FILE *fp)
{
    uint8_t h[10];
    if (fread(h, 1, sizeof(h), fp) != sizeof(h)) {
        return 0;
    }
    if (memcmp(h, "ID3", 3) != 0) {
        return 0;
    }
    const uint32_t sz = ((uint32_t)(h[6] & 0x7F) << 21) | ((uint32_t)(h[7] & 0x7F) << 14) |
                        ((uint32_t)(h[8] & 0x7F) << 7) | (uint32_t)(h[9] & 0x7F);
    return 10 + sz;
}

/*
 * 从第一帧里找 Xing/Info 头，取出总帧数。
 * VBR 文件只有这个能算准时长的；CBR 文件一般没有这个头，走码率估算。
 *
 * 头的位次：帧头 4 字节 + 边信息。边信息长度由"是不是 MPEG1"和声道数决定，
 * 这里用每帧采样数反推 MPEG1：1152 = MPEG1，576 = MPEG2/2.5（两者边信息
 * 长度一样）。这样就不必去碰 helix 内部的版本枚举。
 */
static bool xing_total_frames(const uint8_t *frame, int frame_bytes, int spf, int channels,
                             uint32_t *out_frames)
{
    const bool mpeg1 = (spf == 1152);
    const int side_info = mpeg1 ? (channels == 1 ? 17 : 32) : (channels == 1 ? 9 : 17);
    const int off = 4 + side_info;
    if (frame_bytes < off + 12) {
        return false;
    }
    if (memcmp(frame + off, "Xing", 4) != 0 && memcmp(frame + off, "Info", 4) != 0) {
        return false;
    }
    const uint8_t *p = frame + off + 4;
    const uint32_t flags = ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
                           ((uint32_t)p[2] << 8) | (uint32_t)p[3];
    if ((flags & 0x1) == 0) {
        return false;                       /* 没有帧数字段 */
    }
    p += 4;
    *out_frames = ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
                  ((uint32_t)p[2] << 8) | (uint32_t)p[3];
    return (*out_frames > 0);
}

/* 单声道原地转立体声：从后往前搬，避免覆盖还没读的采样 */
static void mono_to_stereo(int16_t *pcm, int frames)
{
    for (int i = frames - 1; i >= 0; i--) {
        pcm[i * 2]     = pcm[i];
        pcm[i * 2 + 1] = pcm[i];
    }
}

/* 写 PCM。允许短写（I2S 缓冲满时 i2s_channel_write 会少写一部分），
 * 但一个字节都写不进去就报错返回，免得死循环 */
static esp_err_t write_pcm(const int16_t *pcm, size_t bytes)
{
    const uint8_t *p = (const uint8_t *)pcm;
    while (bytes > 0) {
        size_t written = 0;
        esp_err_t err = bsp_audio_write((void *)p, bytes, &written, 2000);
        if (err != ESP_OK && err != ESP_ERR_TIMEOUT) {
            return err;
        }
        if (written == 0) {
            return ESP_ERR_TIMEOUT;
        }
        p += written;
        bytes -= written;
    }
    return ESP_OK;
}

/*---------------------------------------------------------------------------
 * 供给统计（排查"滋滋"杂音用）
 *
 * 每 5 秒打一行：这段时间里送出了多少**音频毫秒**、平均每帧在
 * i2s_channel_write 上阻塞了多久。
 *
 * 怎么读这一行：
 *   音频毫秒 ≈ 实际经过的毫秒，且平均阻塞 ≈ 一帧的播放时长（44.1kHz 下
 *   1152 帧 ≈ 26ms）→ 供给跟得上，DMA 环一直是满的；
 *   音频毫秒明显少于实际毫秒、阻塞接近 0 → 我们把 DMA 喂空了，喇叭放的是
 *   auto_clear 补的零，听感就是断断续续的"滋滋"。
 *
 * 加这一行的原因：**"解码/读盘跟不上实时"和"时钟抖动/削顶"听感都是杂音，
 * 但改法南辕北辙**，必须先用数据分开。
 *-------------------------------------------------------------------------*/
static uint32_t s_stat_frames;      /* 累计送出的每声道帧数 */
static uint32_t s_stat_chunks;
static uint64_t s_stat_write_us;
static int64_t  s_stat_start;

static void stat_reset(void)
{
    s_stat_frames = 0;
    s_stat_chunks = 0;
    s_stat_write_us = 0;
    s_stat_start = 0;
}

static void stat_note_write(uint32_t write_us, int frames)
{
    s_stat_write_us += write_us;
    s_stat_chunks++;
    s_stat_frames += (uint32_t)frames;

    const int64_t now = esp_timer_get_time();
    if (s_stat_start == 0) {
        s_stat_start = now;
        return;
    }
    const uint32_t wall_ms = (uint32_t)((now - s_stat_start) / 1000);
    if (wall_ms < 5000 || s_rate == 0) {
        return;
    }

    const uint32_t audio_ms = (uint32_t)((uint64_t)s_stat_frames * 1000 / s_rate);
    const uint32_t avg_write = (uint32_t)(s_stat_write_us / s_stat_chunks);
    const uint32_t frame_us = (uint32_t)(1000000ull * 1152 / s_rate);
    ESP_LOGI(TAG, "供给：音频 %u ms / 实际 %u ms，平均每帧写入阻塞 %u us（一帧约 %u us）",
             (unsigned)audio_ms, (unsigned)wall_ms, (unsigned)avg_write, (unsigned)frame_us);

    s_stat_frames = 0;
    s_stat_chunks = 0;
    s_stat_write_us = 0;
    s_stat_start = now;
}

/*
 * 放一首歌，直到放完 / 被停止 / 出错。
 * 返回 true 表示正常放完，*next_out 是下一首的序号（-1 = 列表放完了）；
 * 返回 false 表示被停止或出错，调用方应当结束本次播放。
 */
static bool play_file(int index, int *next_out)
{
    char path[sizeof(APP_MUSIC_DIR) + APP_AUDIO_NAME_LEN + 2];
    snprintf(path, sizeof(path), "%s/%s", APP_MUSIC_DIR, s_names[index]);

    FILE *fp = fopen(path, "rb");
    if (fp == NULL) {
        ESP_LOGE(TAG, "打不开 %s", path);
        set_state(APP_AUDIO_ERROR);
        return false;
    }

    if (s_in_buf == NULL) {
        s_in_buf = malloc(MP3_BUF_BYTES + MP3_BUF_PAD);
        if (s_in_buf != NULL) {
            memset(s_in_buf + MP3_BUF_BYTES, 0, MP3_BUF_PAD);   /* 见 MP3_BUF_PAD */
        }
    }
    if (s_in_buf == NULL || decoder_reset() != ESP_OK) {
        ESP_LOGE(TAG, "解码器内存不足");
        fclose(fp);
        set_state(APP_AUDIO_ERROR);
        return false;
    }

    /* 文件大小与音频起点：seek 的比例换算、时长估算都要用 */
    fseek(fp, 0, SEEK_END);
    s_file_size = (uint32_t)ftell(fp);
    fseek(fp, 0, SEEK_SET);
    s_audio_start = id3v2_bytes(fp);
    if (s_audio_start >= s_file_size) {
        s_audio_start = 0;
    }
    fseek(fp, (long)s_audio_start, SEEK_SET);

    ESP_LOGI(TAG, "[%d/%d] %s（%u 字节，ID3 偏移 %u）",
             index + 1, s_track_count, s_names[index],
             (unsigned)s_file_size, (unsigned)s_audio_start);

    /* 缓冲状态。read_ptr 之后是还没解码的数据 */
    uint8_t *read_ptr = s_in_buf;
    int      buf_used = 0;          /* 缓冲里的有效字节数 */
    bool     eof = false;
    bool     first = true;          /* 还没解出第一帧 */
    bool     paused = false;
    int32_t  seek_to = -1;
    bool     ok = true;             /* 正常放完 = true */
    bool     next_set = false;      /* *next_out 是否已经被"切歌"那条路填过了 */
    int      bad_frames = 0;        /* 本文件跳过的坏帧数，日志限流用 */

    s_frames_done = 0;
    s_pos_ms = 0;
    s_duration_ms = 0;
    s_index = index;
    stat_reset();                       /* 供给统计按文件重新开始算 */

    while (!eof) {
        /* ---- 1. 处理命令 ---- */
        audio_cmd_t cmd;
        bool switch_to = false;
        int  next_index = index;
        while (xQueueReceive(s_cmd_q, &cmd, 0) == pdTRUE) {
            switch (cmd.op) {
            case CMD_PAUSE:
                if (!paused) {
                    paused = true;
                    /* 暂停时第一帧还没解出来的话，界面随后会收到 PLAYING，
                     * 顺序反了也无所谓——两边显示的都是当前真实状态 */
                    set_state(APP_AUDIO_PAUSED);
                }
                break;
            case CMD_RESUME:
                if (paused) {
                    paused = false;
                    set_state(APP_AUDIO_PLAYING);
                }
                break;
            case CMD_TOGGLE:
                paused = !paused;
                set_state(paused ? APP_AUDIO_PAUSED : APP_AUDIO_PLAYING);
                break;
            case CMD_STOP:
                if (cmd.stop_id != 0) {
                    s_playback_stop_id = cmd.stop_id;
                }
                ok = false;
                goto done;
            case CMD_SEEK:
                seek_to = cmd.value;
                break;
            case CMD_PLAY:
                next_index = (int)cmd.value;
                switch_to = true;
                break;
            default:
                break;
            }
        }
        if (switch_to) {
            *next_out = (next_index >= 0 && next_index < s_track_count) ? next_index : -1;
            next_set = true;
            goto done;
        }

        if (paused) {
            vTaskDelay(pdMS_TO_TICKS(20));
            continue;
        }

        /* ---- 2. seek ---- */
        if (seek_to >= 0) {
            const uint32_t target = (seek_to > (int32_t)s_duration_ms) ? s_duration_ms
                                                                      : (uint32_t)seek_to;
            const uint64_t span = (uint64_t)(s_file_size - s_audio_start);
            const long pos = (long)(s_audio_start +
                                    (s_duration_ms > 0 ? span * target / s_duration_ms : 0));
            if (fseek(fp, pos, SEEK_SET) != 0 || decoder_reset() != ESP_OK) {
                ESP_LOGE(TAG, "seek 失败");
                ok = false;
                goto done;
            }
            buf_used = 0;
            read_ptr = s_in_buf;
            eof = false;
            /* 位置先按请求值填上，随后的解码会把它修正到真实位置 */
            s_frames_done = (uint64_t)target * s_rate / 1000;
            s_pos_ms = target;
            seek_to = -1;
        }

        /* ---- 3. 补数据 ---- */
        int left = buf_used - (int)(read_ptr - s_in_buf);
        if (left < MAINBUF_SIZE * 5 / 4 && !eof) {
            memmove(s_in_buf, read_ptr, left);
            const size_t n = fread(s_in_buf + left, 1, MP3_BUF_BYTES - left, fp);
            if (n == 0 || feof(fp)) {
                eof = true;
            }
            buf_used = left + (int)n;
            read_ptr = s_in_buf;
            left = buf_used;
        }

        /* ---- 4. 找同步字、解码一帧 ---- */
        const int offset = MP3FindSyncWord(read_ptr, left);
        if (offset < 0) {
            if (eof) {
                break;                      /* 尾巴上的碎片，正常结束 */
            }
            /* 这一段里没有同步字：丢掉，只留 1 个字节防止同步字跨在两次
             * 读盘之间被切断。（buf_used 不用动：它记的是"从 s_in_buf 起
             * 有多少有效字节"，读指针前移多少，剩余长度就少多少） */
            const int drop = (left > 1) ? left - 1 : 0;
            read_ptr += drop;
            continue;
        }
        read_ptr += offset;
        left -= offset;

        const uint8_t *frame_start = read_ptr;      /* MP3Decode 会推进它 */
        const int frame_avail = left;
        int unread = left;
        const int derr = MP3Decode(s_dec, &read_ptr, &unread, s_pcm, 0);

        /*
         * 剩余长度**不用** helix 返回的 unread，按缓冲区自己的几何算：
         * 它的错误路径里"指针推进"和"bytesLeft 扣减"并不同步——
         *   INVALID_FRAMEHEADER：在 `*inbuf += fhBytes` 之前就 return；
         *   INVALID_SIDEINFO：推进了指针，却没执行 `*bytesLeft -= ...`。
         * 拿它当账本会和缓冲区对不上。解码只让读指针前移，"从文件读进来多少
         * 字节"（buf_used）不受影响，所以每轮开头用
         *   left = buf_used - (read_ptr - s_in_buf)
         * 重新算一遍就够了（见循环开头那行）。
         */
        (void)unread;

        if (derr == ERR_MP3_MAINDATA_UNDERFLOW) {
            continue;                       /* 数据不够一帧，回去补数据 */
        }
        if (derr != ERR_MP3_NONE) {
            if (eof) {
                break;                      /* 文件末尾的残帧，当正常结束 */
            }
            /*
             * 必须保证读指针至少前进一步。
             *
             * MP3FindSyncWord 只认"11~12 个同步位"，而 UnpackFrameHeader 还会
             * 拒掉 layer / srIdx / brIdx 非法的头。于是存在这样的字节组合：
             * FindSyncWord 认它，MP3Decode 却在**推进读指针之前**就报
             * INVALID_FRAMEHEADER 返回。不强制前进的话，下一轮从同一位置再找
             * 一个同步字——还是同一个假头——无限空转：声音卡住、日志刷屏，
             * 而界面上的进度冻在那里还显示"正在播放"。它不是死机（循环顶上
             * 仍在收命令，停止/切歌能出来），但播放自己永远回不来。
             *
             * 拖进度条落到帧数据中间时这条路是常态：seek 按"时间→字节"线性
             * 换算，落点几乎不可能正好是帧边界，而 0xFF 后跟高位字节的假同步
             * 字在压缩数据里很常见。损坏文件、后缀改成 .mp3 的别的文件同理。
             */
            if (read_ptr == frame_start) {
                read_ptr++;
            }
            /*
             * 日志限流。串口日志是阻塞写，一条约 3.5ms（115200 波特）；
             * 整段数据都是坏帧时（损坏文件、或 seek 落进一大片垃圾），
             * 逐条打印会把跳过过程拖成几十秒的卡顿——排查用的日志反而
             * 成了故障本身。
             */
            if (bad_frames < 3) {
                ESP_LOGW(TAG, "跳过坏帧(%d) @%ums", derr, (unsigned)s_pos_ms);
            } else if (bad_frames == 3) {
                ESP_LOGW(TAG, "坏帧太多，后续不再逐条打印");
            }
            bad_frames++;
            continue;
        }

        MP3FrameInfo info;
        MP3GetLastFrameInfo(s_dec, &info);
        if (info.nChans <= 0 || info.samprate <= 0) {
            continue;
        }

        if (first) {
            first = false;
            s_rate = (uint32_t)info.samprate;
            const int spf = info.outputSamps / info.nChans;

            /* 时长：优先 Xing 总帧数，否则按码率估 */
            uint32_t frames = 0;
            if (xing_total_frames(frame_start, frame_avail, spf, info.nChans, &frames)) {
                s_duration_ms = (uint32_t)((uint64_t)frames * spf * 1000 / s_rate);
                ESP_LOGI(TAG, "Xing: %u 帧, %d Hz → %u ms", (unsigned)frames,
                         (int)s_rate, (unsigned)s_duration_ms);
            } else if (info.bitrate > 0) {
                s_duration_ms = (uint32_t)((uint64_t)(s_file_size - s_audio_start) * 8 / info.bitrate);
                ESP_LOGI(TAG, "按码率估时长：%d kbps → %u ms", info.bitrate, (unsigned)s_duration_ms);
            }

            /*
             * 切采样率。同一个采样率的歌连着放时这次调用是空操作（BSP 里
             * 比对过就直接返回），跨采样率才会真的重配 I2S 与 ES8311。
             */
            esp_err_t ferr = bsp_audio_set_fs(s_rate, 16, 2);
            if (ferr != ESP_OK) {
                ESP_LOGE(TAG, "切换到 %u Hz 失败: %s", (unsigned)s_rate, esp_err_to_name(ferr));
                set_state(APP_AUDIO_ERROR);
                ok = false;
                goto done;
            }

            /* 第一帧解出来了才算真的在放，这时候再通知界面：标题、时长、
             * 采样率都齐了，界面一次就能把这一屏填对 */
            set_state(APP_AUDIO_PLAYING);
        }

        /* ---- 5. 送 I2S ---- */
        int frames = info.outputSamps / info.nChans;
        size_t bytes = (size_t)info.outputSamps * sizeof(int16_t);
        if (info.nChans == 1) {
            mono_to_stereo(s_pcm, frames);  /* ES8311 这条通路只吃立体声 */
            bytes *= 2;
        }
        /*
         * 频谱灯的抽头：**送 I2S 之前**把这一帧原样让出去一份。
         * 挂在写之前而不是之后，是因为 write_pcm 会阻塞到 DMA 收下数据
         * （一首歌放完时更明显），挂后面的话灯会跟着声音一起卡。
         *
         * 回调必须立刻返回：app_led2812 那边只做一次拷贝进环形缓冲，
         * 真正的分析跑在它自己的任务里（见 app_led2812.c）。
         */
        if (s_pcm_cb != NULL) {
            s_pcm_cb(s_pcm, (size_t)frames, s_rate, s_pcm_user);
        }

        const int64_t t_write = esp_timer_get_time();
        if (write_pcm(s_pcm, bytes) != ESP_OK) {
            ESP_LOGE(TAG, "I2S 写入失败");
            set_state(APP_AUDIO_ERROR);
            ok = false;
            goto done;
        }
        stat_note_write((uint32_t)(esp_timer_get_time() - t_write), frames);

        s_frames_done += (uint64_t)frames;
        s_pos_ms = (uint32_t)(s_frames_done * 1000 / s_rate);
    }

done:
    /*
     * **自然放完就接着放下一首。** 这一句以前没有，所以"自动连播"从来没生效过：
     * *next_out 只在按了下一首/上一首那条路上被赋值，自然结束时它还是调用方
     * 初始化的 -1，run_playback 就当成"播放列表放完了"直接收摊。界面上看着
     * 一切正常（进度条走到头、状态回到 IDLE），只是歌不放第二首。
     *
     * **列表放完就停**，不绕回第一首——按了"下一首"按钮才是绕回的（见
     * app_audio_play_next）。想让它循环的话，这里改成 `(index + 1) % s_track_count`
     * 就行，但那样就没法靠"让它放完"停下来了。
     */
    if (ok && !next_set) {
        *next_out = (index + 1 < s_track_count) ? index + 1 : -1;
    }

    fclose(fp);
    read_ptr = NULL;
    buf_used = 0;
    return ok;
}

static esp_err_t release_audio_resources(void)
{
    const esp_err_t err = bsp_audio_deinit();
    if (s_pcm != NULL) {
        heap_caps_free(s_pcm);
        s_pcm = NULL;
    }
    if (s_dec != NULL) {
        MP3FreeDecoder(s_dec);
        s_dec = NULL;
    }
    free(s_in_buf);
    s_in_buf = NULL;
    return err;
}

/* 播放期间一直开着功放（歌与歌之间不关），避免每首都"啪"一下 */
static void run_playback(int index)
{
    if (s_track_count <= 0) {
        set_state(APP_AUDIO_IDLE);
        return;
    }
    if (index < 0 || index >= s_track_count) {
        index = 0;
    }

    s_playback_stop_id = 0;
    if (s_pcm == NULL) {
        s_pcm = heap_caps_malloc(PCM_BUF_BYTES, MALLOC_CAP_DMA);
        if (s_pcm == NULL) {
            ESP_LOGE(TAG, "DMA 内存不足，PCM 缓冲 (%d 字节) 分配失败", PCM_BUF_BYTES);
            set_state(APP_AUDIO_ERROR);
            return;
        }
    }

    esp_err_t err = bsp_audio_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "音频初始化失败: %s", esp_err_to_name(err));
        heap_caps_free(s_pcm);
        s_pcm = NULL;
        s_index = index;
        set_state(APP_AUDIO_ERROR);
        return;
    }
    /* bsp_audio_init 内部会把音量重置成 BSP 的默认值，这里补回我们的 */
    apply_volume();

    bsp_pa_en(1);

    while (index >= 0) {
        int next = -1;
        if (!play_file(index, &next)) {
            break;
        }
        if (next < 0) {
            ESP_LOGI(TAG, "播放列表放完了");
            break;
        }
        index = next;
    }

    /* 停止后 DMA 里还剩一圈没放完，等它出来再关功放，否则结尾会有一声"咔" */
    vTaskDelay(pdMS_TO_TICKS(120));
    bsp_pa_en(0);

    s_pos_ms = 0;
    /* 出错时保留 ERROR 让界面去提示，正常结束才回到 IDLE */
    if (s_state != APP_AUDIO_ERROR) {
        set_state(APP_AUDIO_IDLE);
    }
    if (s_playback_stop_id != 0) {
        s_stop_result = release_audio_resources();
        s_stop_ack_id = s_playback_stop_id;
        s_playback_stop_id = 0;
    }
}

static void audio_task(void *arg)
{
    (void)arg;
    for (;;) {
        audio_cmd_t cmd;
        if (xQueueReceive(s_cmd_q, &cmd, portMAX_DELAY) != pdTRUE) {
            continue;
        }
        switch (cmd.op) {
        case CMD_PLAY:
            run_playback((int)cmd.value);
            break;
        case CMD_STOP:
            set_state(APP_AUDIO_IDLE);
            if (cmd.stop_id != 0) {
                s_stop_result = release_audio_resources();
                s_stop_ack_id = cmd.stop_id;
            }
            break;
        case CMD_RESUME:
        case CMD_TOGGLE:
            /* 空闲时这两个都是"接着放当前这首" */
            if (s_index >= 0) {
                run_playback(s_index);
            }
            break;
        default:
            /* 空闲状态下暂停 / 停止 / seek 没有意义，直接丢掉 */
            break;
        }
    }
}

/*---------------------------------------------------------------------------
 * 对外接口
 *-------------------------------------------------------------------------*/
esp_err_t app_audio_init(void)
{
    if (s_task != NULL) {
        return ESP_OK;
    }

    s_cmd_q = xQueueCreate(6, sizeof(audio_cmd_t));
    if (s_cmd_q == NULL) {
        return ESP_ERR_NO_MEM;
    }
    s_stop_mutex = xSemaphoreCreateMutex();
    if (s_stop_mutex == NULL) {
        vQueueDelete(s_cmd_q);
        s_cmd_q = NULL;
        return ESP_ERR_NO_MEM;
    }
    if (xTaskCreatePinnedToCore(audio_task, "app_audio", AUDIO_TASK_STACK, NULL,
                                AUDIO_TASK_PRIO, &s_task, AUDIO_TASK_CORE) != pdPASS) {
        vSemaphoreDelete(s_stop_mutex);
        s_stop_mutex = NULL;
        vQueueDelete(s_cmd_q);
        s_cmd_q = NULL;
        s_task = NULL;
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

void app_audio_set_event_cb(app_audio_event_cb_t cb, void *user)
{
    s_event_cb = cb;
    s_event_user = user;
}

void app_audio_set_pcm_cb(app_audio_pcm_cb_t cb, void *user)
{
    s_pcm_cb = cb;
    s_pcm_user = user;
}

esp_err_t app_audio_play_index(int index)
{
    if (s_track_count <= 0) {
        return ESP_ERR_INVALID_STATE;
    }
    if (index < 0) {
        index = 0;
    } else if (index >= s_track_count) {
        index = s_track_count - 1;
    }
    return send_cmd(CMD_PLAY, index);
}

esp_err_t app_audio_play_next(void)
{
    if (s_track_count <= 0) {
        return ESP_ERR_INVALID_STATE;
    }
    int next = s_index + 1;
    if (next >= s_track_count) {
        next = 0;                           /* 最后一首再按下一首就绕回第一首 */
    }
    return send_cmd(CMD_PLAY, next);
}

esp_err_t app_audio_play_prev(void)
{
    if (s_track_count <= 0) {
        return ESP_ERR_INVALID_STATE;
    }
    int prev = s_index - 1;
    if (prev < 0) {
        prev = s_track_count - 1;
    }
    return send_cmd(CMD_PLAY, prev);
}

esp_err_t app_audio_toggle(void)
{
    if (s_state == APP_AUDIO_PLAYING || s_state == APP_AUDIO_PAUSED) {
        return send_cmd(CMD_TOGGLE, 0);
    }
    if (s_index < 0 && s_track_count > 0) {
        s_index = 0;
    }
    return send_cmd(CMD_RESUME, 0);
}

esp_err_t app_audio_pause(void)
{
    return send_cmd(CMD_PAUSE, 0);
}

esp_err_t app_audio_resume(void)
{
    return send_cmd(CMD_RESUME, 0);
}

esp_err_t app_audio_stop(void)
{
    return send_cmd(CMD_STOP, 0);
}

esp_err_t app_audio_stop_and_wait(uint32_t timeout_ms)
{
    if (s_cmd_q == NULL) {
        return release_audio_resources();
    }
    const TickType_t timeout_ticks = pdMS_TO_TICKS(timeout_ms);
    const TickType_t start = xTaskGetTickCount();
    if (xSemaphoreTake(s_stop_mutex, timeout_ticks) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    if ((xTaskGetTickCount() - start) >= timeout_ticks) {
        xSemaphoreGive(s_stop_mutex);
        return ESP_ERR_TIMEOUT;
    }

    uint32_t stop_id = ++s_next_stop_id;
    if (stop_id == 0) {
        stop_id = ++s_next_stop_id;
    }
    const audio_cmd_t cmd = { .op = CMD_STOP, .value = 0, .stop_id = stop_id };
    if (xQueueSend(s_cmd_q, &cmd, 0) != pdTRUE) {
        xSemaphoreGive(s_stop_mutex);
        return ESP_ERR_TIMEOUT;
    }

    while (s_stop_ack_id != stop_id) {
        const TickType_t elapsed = xTaskGetTickCount() - start;
        if (elapsed >= timeout_ticks) {
            xSemaphoreGive(s_stop_mutex);
            return ESP_ERR_TIMEOUT;
        }
        TickType_t remaining = timeout_ticks - elapsed;
        const TickType_t poll_ticks = pdMS_TO_TICKS(10);
        if (poll_ticks != 0 && remaining > poll_ticks) {
            remaining = poll_ticks;
        }
        vTaskDelay(remaining);
    }

    xSemaphoreGive(s_stop_mutex);
    return s_stop_result;
}

esp_err_t app_audio_seek_ms(uint32_t ms)
{
    return send_cmd(CMD_SEEK, (int32_t)ms);
}

/*
 * 把界面上的 0~100 映射到 BSP 的 0~93 再写下去。
 *
 * 为什么要映射而不是直接传：BSP 那边 93 就是 0dBFS，93~100 是**数字增益**
 * （100 = +3.6dB），会被夹住。直接传的话滑条最后 7% 是"拖了没反应"的死区；
 * 铺开之后滑条每一格都有效，而且顶上就是干净的满刻度。
 * 见 lckfb_bsp.h 里 BSP_AUDIO_VOLUME_MAX 的推导。
 */
static void apply_volume(void)
{
    bsp_audio_volume_set(s_volume * BSP_AUDIO_VOLUME_MAX / 100);
}

esp_err_t app_audio_set_volume(int volume)
{
    if (volume < 0) {
        volume = 0;
    } else if (volume > 100) {
        volume = 100;
    }
    s_volume = volume;
    /*
     * 直接写，不走命令队列：音量滑条一拖就是几十上百个事件，队列只有 6 格，
     * 丢几个就会出现"手停下来了音量还在中间"的错位。BSP 那边的 I2C 写有
     * 驱动自带的互斥，从界面线程写是安全的。
     * （BSP 自己也存了一份音量，切采样率重开 codec 时会补回来）
     */
    apply_volume();
    return ESP_OK;
}

esp_err_t app_audio_get_volume(int *volume)
{
    if (volume == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    *volume = s_volume;
    return ESP_OK;
}

app_audio_state_t app_audio_state(void)
{
    return s_state;
}

int app_audio_current_index(void)
{
    return s_index;
}

uint32_t app_audio_position_ms(void)
{
    return s_pos_ms;
}

uint32_t app_audio_duration_ms(void)
{
    return s_duration_ms;
}
