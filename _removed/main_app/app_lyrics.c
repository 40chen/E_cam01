/*
 * app_lyrics.c — .lrc 歌词解析
 *
 * 编码：先看 BOM，再看整块字节是不是合法 UTF-8；不是就按 GB2312 查表转。
 *   为什么要这么判：GB2312 的汉字是两个高位字节，其中首字节 0x81~0xBF
 *   在 UTF-8 里根本不合法，所以"魔改过的 UTF-8"和 GBK 基本一眼能分开。
 *   查不到的字节（GBK 扩展区、生僻字）输出 '?'——那些字本来也不在 LVGL
 *   内置的 CJK 字库里，转出来也是空白。
 *
 * 内存：歌词文本一块、每行一个时间和偏移。整块从堆上分配（几 KB 到几十
 * KB，按 CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL=2048 会落到 PSRAM）。
 */
#include "app_lyrics.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_log.h"

#include "gb2312_table.h"

static const char *TAG = "app_lyrics";

/* .lrc 文件最大读多少。正常歌词文件几 KB，超过这个量的多半不是歌词 */
#define LRC_MAX_BYTES (64 * 1024)
/* 歌词文本缓冲（转换之后的 UTF-8） */
#define LRC_TEXT_BYTES (48 * 1024)
/* 一行里最多几个时间戳（一行多时间戳是 LRC 的合法写法，用来标记重复段） */
#define LRC_MAX_STAMPS 16

static char     *s_text;        /* 所有歌词行，'\0' 分隔 */
static uint16_t *s_off;         /* 每行在 s_text 里的起始偏移 */
static uint32_t *s_time;        /* 每行的起始时间（毫秒） */
static size_t    s_used;        /* s_text 里已经用掉的字节数（含每行结尾的 0） */
static int       s_count;
static char      s_title[64];
static char      s_artist[64];

/*---------------------------------------------------------------------------
 * 编码转换
 *-------------------------------------------------------------------------*/
static int utf8_encode(uint16_t cp, char *out)
{
    if (cp < 0x80) {
        out[0] = (char)cp;
        return 1;
    }
    if (cp < 0x800) {
        out[0] = (char)(0xC0 | (cp >> 6));
        out[1] = (char)(0x80 | (cp & 0x3F));
        return 2;
    }
    out[0] = (char)(0xE0 | (cp >> 12));
    out[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
    out[2] = (char)(0x80 | (cp & 0x3F));
    return 3;
}

static bool utf8_valid(const uint8_t *p, size_t n)
{
    size_t i = 0;
    while (i < n) {
        const uint8_t c = p[i];
        if (c < 0x80) {
            i++;
            continue;
        }
        int len;
        if ((c & 0xE0) == 0xC0) {
            len = 2;
        } else if ((c & 0xF0) == 0xE0) {
            len = 3;
        } else if ((c & 0xF8) == 0xF0) {
            len = 4;
        } else {
            return false;               /* 0x80~0xBF 的孤立字节，或 0xF8+ */
        }
        if (i + (size_t)len > n) {
            return false;               /* 截断在半个字符上 */
        }
        for (int k = 1; k < len; k++) {
            if ((p[i + k] & 0xC0) != 0x80) {
                return false;
            }
        }
        i += (size_t)len;
    }
    return true;
}

/* 原样拷贝 UTF-8，顺手把 '\r' 丢掉（统一成 '\n' 换行） */
static size_t utf8_copy(const uint8_t *in, size_t n, char *out, size_t cap)
{
    size_t o = 0;
    for (size_t i = 0; i < n && o + 1 < cap; i++) {
        if (in[i] == '\r') {
            continue;
        }
        out[o++] = (char)in[i];
    }
    out[o] = '\0';
    return o;
}

/* GB2312/GBK -> UTF-8。查不到的字输出 '?' */
static size_t gb2312_to_utf8(const uint8_t *in, size_t n, char *out, size_t cap)
{
    size_t o = 0;
    for (size_t i = 0; i < n && o + 4 < cap; ) {
        const uint8_t c = in[i];
        if (c < 0x80) {
            i++;
            if (c == '\r') {
                continue;
            }
            out[o++] = (char)c;
            continue;
        }
        /*
         * GBK 的首字节是 0x81~0xFE，尾字节 0x40~0xFE（不含 0x7F）。落在
         * GB2312 之外的那些（扩展区汉字）表里查不到，但**两个字节都要吃掉**：
         * 只吃首字节的话，尾字节会当成 ASCII 漏到屏幕上，多出一个乱码字符
         * （比如扩展字 "髙" 的尾字节是 0x7B，就冒出一个 '{'）。
         */
        if (i + 1 < n && c >= 0x81 && c <= 0xFE) {
            const uint8_t trail = in[i + 1];
            if (c >= 0xA1 && c <= 0xF7 && trail >= 0xA1 && trail <= 0xFE) {
                const uint16_t cp = gb2312_unicode_table[(c - 0xA1) * GB2312_TABLE_COLS +
                                                         (trail - 0xA1)];
                if (cp != 0) {
                    o += (size_t)utf8_encode(cp, out + o);
                    i += 2;
                    continue;
                }
            }
            out[o++] = '?';
            i += 2;
            continue;
        }
        out[o++] = '?';
        i++;
    }
    out[o] = '\0';
    return o;
}

/*---------------------------------------------------------------------------
 * 解析
 *-------------------------------------------------------------------------*/
/* 解析 "[mm:ss.xx]"，p 指向 '[' 之后。成功时 *end 指向 ']' 之后 */
static bool parse_stamp(const char *p, const char **end, uint32_t *out_ms)
{
    if (!isdigit((unsigned char)*p)) {
        return false;
    }
    long mm = 0;
    long ss = 0;
    long frac = 0;
    int  frac_digits = 0;

    /*
     * 位数封顶 4 位。手改坏或被别的工具写坏的 .lrc 里可能出现
     * [9999999999:00.00] 这种畸形时间戳，一直累加是有符号溢出的未定义行为；
     * 而 4 位同时也是"算出来的毫秒不会撑破 uint32"的上限
     * （9999 分钟 × 60000 = 6.0e8，离 4.29e9 还很远）。
     * 9999 分钟 ≈ 166 小时，正常歌词远远用不到。
     */
    int digits = 0;
    while (isdigit((unsigned char)*p)) {
        if (digits++ < 4) {
            mm = mm * 10 + (*p - '0');
        }
        p++;
    }
    if (*p++ != ':') {
        return false;
    }
    digits = 0;
    while (isdigit((unsigned char)*p)) {
        if (digits++ < 4) {
            ss = ss * 10 + (*p - '0');
        }
        p++;
    }
    if (*p == '.' || *p == ':') {           /* 小数部分，两种写法都见过 */
        p++;
        while (isdigit((unsigned char)*p)) {
            if (frac_digits < 3) {
                frac = frac * 10 + (*p - '0');
                frac_digits++;
            }
            p++;
        }
    }
    if (*p != ']') {
        return false;
    }
    *end = p + 1;

    uint32_t ms = (uint32_t)mm * 60000u + (uint32_t)ss * 1000u;
    if (frac_digits == 1) {
        ms += (uint32_t)frac * 100u;        /* [mm:ss.x] */
    } else if (frac_digits == 2) {
        ms += (uint32_t)frac * 10u;         /* [mm:ss.xx]，最常见 */
    } else if (frac_digits == 3) {
        ms += (uint32_t)frac;               /* [mm:ss.xxx] */
    }
    *out_ms = ms;
    return true;
}

/* [ti:标题] [ar:歌手] 这些标签，认得出的存下来，认不出的一律忽略 */
static void handle_tag(const char *body, const char *end)
{
    char buf[64];
    const char *colon = memchr(body, ':', (size_t)(end - body));
    if (colon == NULL || colon + 1 >= end) {
        return;
    }
    const size_t body_len = (size_t)(end - (colon + 1));
    if (body_len >= sizeof(buf)) {
        return;
    }
    memcpy(buf, colon + 1, body_len);
    buf[body_len] = '\0';

    if (strncmp(body, "ti:", 3) == 0) {
        strlcpy(s_title, buf, sizeof(s_title));
    } else if (strncmp(body, "ar:", 3) == 0) {
        strlcpy(s_artist, buf, sizeof(s_artist));
    }
    /* [al:] [by:] [offset:] [length:] 等这里不处理 */
}

static void add_line(uint32_t ms, const char *text)
{
    if (s_count >= APP_LYRICS_MAX_LINES) {
        return;
    }
    const size_t len = strlen(text);
    if (s_used + len + 1 > LRC_TEXT_BYTES) {
        return;                             /* 文本缓冲满了，后面的丢掉 */
    }
    if (s_used > 0xFFFF) {
        return;                             /* 偏移是 uint16，越界就不要了 */
    }
    s_off[s_count] = (uint16_t)s_used;
    /* 用 memmove 不用 memcpy：源和目标在**同一块缓冲**里（text 就指在 s_text
     * 中靠后的位置），第一行歌词正文超过 10 个字节时两段就重叠了——dst < src
     * 时常见实现恰好是对的，但这是标准意义上的未定义行为，不值得赌 */
    memmove(s_text + s_used, text, len + 1);
    s_used += len + 1;
    s_time[s_count] = ms;
    s_count++;
}

/* 按时间排序。一个时间戳数组和偏移数组要一起动，所以用最笨的插入排序
 * （400 行最多 8 万次比较，一次性开销，不值得为它写快排） */
static void sort_lines(void)
{
    for (int i = 1; i < s_count; i++) {
        const uint32_t t = s_time[i];
        const uint16_t o = s_off[i];
        int j = i - 1;
        while (j >= 0 && s_time[j] > t) {
            s_time[j + 1] = s_time[j];
            s_off[j + 1] = s_off[j];
            j--;
        }
        s_time[j + 1] = t;
        s_off[j + 1] = o;
    }
}

/* 去掉行首空格（歌词前的缩进没有意义，而且在窄屏上是浪费） */
static const char *skip_spaces(const char *p)
{
    while (*p == ' ' || *p == '\t') {
        p++;
    }
    return p;
}

static void parse(char *text)
{
    char *line = text;
    while (line != NULL && *line != '\0') {
        char *nl = strchr(line, '\n');
        if (nl != NULL) {
            *nl = '\0';
        }

        const char *p = line;
        uint32_t stamps[LRC_MAX_STAMPS];
        int nstamps = 0;

        /* 行首可以挂多个标签：[00:12.34][01:20.00]歌词 */
        while (*p == '[') {
            const char *end = NULL;
            uint32_t t = 0;
            if (parse_stamp(p + 1, &end, &t)) {
                if (nstamps < LRC_MAX_STAMPS) {
                    stamps[nstamps++] = t;
                }
                p = end;
                continue;
            }
            /* 不是时间戳：当成 [xx:yy] 标签吃掉，吃到 ']' 为止 */
            const char *close = strchr(p, ']');
            if (close == NULL) {
                break;
            }
            handle_tag(p + 1, close);
            p = close + 1;
        }

        if (nstamps > 0) {
            const char *lyric = skip_spaces(p);
            for (int i = 0; i < nstamps; i++) {
                add_line(stamps[i], lyric);
            }
        }

        line = (nl != NULL) ? nl + 1 : NULL;
    }
}

/*---------------------------------------------------------------------------
 * 对外接口
 *-------------------------------------------------------------------------*/
void app_lyrics_free(void)
{
    free(s_text);
    free(s_off);
    free(s_time);
    s_text = NULL;
    s_off = NULL;
    s_time = NULL;
    s_used = 0;
    s_count = 0;
    s_title[0] = '\0';
    s_artist[0] = '\0';
}

static bool ends_with(const char *s, const char *suffix)
{
    const size_t n = strlen(s);
    const size_t m = strlen(suffix);
    return (n >= m) && (strcasecmp(s + n - m, suffix) == 0);
}

esp_err_t app_lyrics_load(const char *mp3_path)
{
    app_lyrics_free();

    if (mp3_path == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    /* 同名换后缀：把结尾的 .mp3/.MP3 去掉，接上 .lrc
     * （FAT 本身大小写不敏感，所以小写 .lrc 能打开 "歌.LRC"） */
    char path[160];
    strlcpy(path, mp3_path, sizeof(path));
    if (ends_with(path, ".mp3")) {
        path[strlen(path) - 4] = '\0';
    }
    if (strlcat(path, ".lrc", sizeof(path)) >= sizeof(path)) {
        return ESP_ERR_INVALID_ARG;         /* 路径太长，装不下 */
    }

    FILE *fp = fopen(path, "rb");
    if (fp == NULL) {
        /* 没有同名 .lrc 是常态（界面上就显示黑胶），但日志里说一声：
         * "为什么一直是黑胶"这个问题，靠日志一眼能看出来，不用猜 */
        ESP_LOGI(TAG, "没有歌词文件：%s", path);
        return ESP_ERR_NOT_FOUND;
    }

    uint8_t *raw = malloc(LRC_MAX_BYTES);
    char    *text = malloc(LRC_TEXT_BYTES);
    s_off = malloc(APP_LYRICS_MAX_LINES * sizeof(uint16_t));
    s_time = malloc(APP_LYRICS_MAX_LINES * sizeof(uint32_t));
    if (raw == NULL || text == NULL || s_off == NULL || s_time == NULL) {
        free(raw);
        free(text);
        fclose(fp);
        app_lyrics_free();
        ESP_LOGE(TAG, "内存不足");
        return ESP_ERR_NO_MEM;
    }

    const size_t nread = fread(raw, 1, LRC_MAX_BYTES, fp);
    fclose(fp);

    /*
     * 编码识别：BOM > 合法 UTF-8 > GB2312。
     *
     * BOM 这里用**循环**去掉而不是只查一次：见过编辑器在已经带 BOM 的文件上
     * 再加一个，剩下那个 BOM 会顶在第一行最前面，让"行首是 '[' 才算歌词"的
     * 判断认不出这一行——症状是第一句歌词无声消失，很难往编码上想。
     */
    size_t start = 0;
    while (start + 3 <= nread && raw[start] == 0xEF && raw[start + 1] == 0xBB &&
           raw[start + 2] == 0xBF) {
        start += 3;
    }
    const bool is_utf8 = (start > 0) || utf8_valid(raw, nread);

    if (is_utf8) {
        utf8_copy(raw + start, nread - start, text, LRC_TEXT_BYTES);
    } else {
        gb2312_to_utf8(raw, nread, text, LRC_TEXT_BYTES);
    }
    free(raw);

    s_text = text;
    s_used = 0;
    s_count = 0;
    parse(s_text);
    sort_lines();

    ESP_LOGI(TAG, "%s：%s，%d 行歌词", path, is_utf8 ? "UTF-8" : "GB2312/GBK", s_count);

    if (s_count == 0) {
        app_lyrics_free();
        return ESP_ERR_NOT_FOUND;           /* 有文件但一行时间戳都没有 */
    }
    return ESP_OK;
}

bool app_lyrics_ready(void)
{
    return s_text != NULL && s_count > 0;
}

int app_lyrics_count(void)
{
    return s_count;
}

const char *app_lyrics_line(int index)
{
    if (s_text == NULL || index < 0 || index >= s_count) {
        return "";
    }
    return s_text + s_off[index];
}

const char *app_lyrics_title(void)
{
    return s_title;
}

const char *app_lyrics_artist(void)
{
    return s_artist;
}

int app_lyrics_index_at(uint32_t ms)
{
    if (s_time == NULL || s_count == 0) {
        return -1;
    }
    /* 行数少，直接线性扫；歌词是顺序播放的，实际上每次只挪一两行。
     * 要是有性能顾虑，这里可以记一个上次的位置往前找，但现在不值得 */
    int found = -1;
    for (int i = 0; i < s_count; i++) {
        if (s_time[i] > ms) {
            break;
        }
        found = i;
    }
    return found;
}
