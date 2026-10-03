/*
 * app_led2812.c — WS2812 8x8 氛围灯
 *
 * WS2812 的时序用 RMT 自己编码，**不引 espressif/led_strip 那个托管组件**：
 * 它的作用就是把下面 strip_send() 这几十行包一层，而本工程一贯不要这种
 * 薄封装（自己的 MP3 解码循环同理）。少一个依赖就少一处版本要跟着 IDF 走
 * 的地方——led_strip 3.x 就要求 IDF 5.3+ 拆出来的 esp_driver_rmt，而本工程
 * 锁在 5.1.7。
 *
 * 时序（WS2812B，±150ns）：
 *   T0H 0.4µs / T0L 0.85µs      T1H 0.8µs / T1L 0.45µs      周期 1.25µs
 * 取 10MHz 的 RMT 分辨率（1 tick = 0.1µs），四项分别是 4/9/8/5 tick，
 * 周期 13 tick = 1.3µs，全在容差内。**复位靠帧与帧之间的空闲低电平**
 * （>50µs），RMT 空闲电平默认就是 0，两帧之间隔着三十多毫秒，够得不能再够。
 *
 * 一次刷新 = 64 颗 x 24bit = 1536 个 RMT 符号，远超一颗通道 48 字的 RMT 内存。
 * 这不是问题：新 RMT 驱动的非 DMA 路径是**乒乓补齐**的（rmt_tx.c 把内存劈成
 * 两半，靠 TX_THRESH 中断续灌），所以一次 rmt_transmit 把整帧丢进去就行，
 * 不要自己按块切——在 WS2812 的数据流中间人为断开是没定义的。
 */
#include "app_led2812.h"

#include <math.h>
#include <string.h>

#include "driver/rmt_encoder.h"
#include "driver/rmt_tx.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "app_audio.h"
#include "app_module.h"

static const char *TAG = "led2812";

/* 板子的规格在头文件里（APP_LED_*），这里是内部用的短名字 */
#define LED_COLS    APP_LED_COLS
#define LED_ROWS    APP_LED_ROWS

/*---------------------------------------------------------------------------
 * 规格
 *-------------------------------------------------------------------------*/
#define LED_RMT_RES_HZ      (10 * 1000 * 1000)      /* 1 tick = 0.1µs */

/* RMT 内存块。S3 是每通道 48 字，请求 4 块（192 字）能把乒乓的半场撑到
 * 96 个符号 ≈ 125µs，中断不会追着每一颗灯跑。**本工程没有别人用 RMT**，
 * 所以这 4 块一定拿得到；拿不到就往下退，最小 48（一块）。 */
#define LED_MEM_BLOCK_MAX   192
#define LED_MEM_BLOCK_MIN   48
#define LED_MEM_BLOCK_STEP  48

/* 整帧电流上限（毫安）。64 颗满白不封顶是 3.8A，5V 轨和线都受不了。
 * 参考项目用 1000，这里更保守。**注意这会让满白被压到 19% 左右**，
 * 所以亮度滑条是必要的第二道闸，默认只给 40。 */
#define LED_MAX_MA          800
#define LED_MA_IDLE         1       /* 每颗静态电流 */
#define LED_MA_PER_CH       20      /* 每个通道满亮的电流 */

/* 频谱分析的窗长。512 点在 16kHz 下是 32ms，正好和 30fps 的刷新节拍对齐；
 * 44.1kHz 下是 11.6ms，一样能用（频段边界按采样率重算，不做重采样）。 */
#define LED_FFT_N           512
#define LED_BANDS           APP_LED_COLS

/* 频谱的压缩参数，起点照抄参考项目（esp32s3supermini-audio-spectrum-light），
 * **上板要按实际电平重调**：它喂的是麦克风，我们喂的是解码后的满幅 PCM，
 * 同一组常数在两条链路上的量纲不一样。 */
#define LED_SENSITIVITY     12.0f
#define LED_LOG_FULL_SCALE  16.0f
#define LED_F_LO            100.0f  /* 最低频段的下沿 */
#define LED_F_HI            6400.0f /* 最高频段的上沿（会再按采样率夹一次） */

/* 落柱速度：参考项目是 10ms 落一行，我们一帧 33ms，所以一帧落 3 行。
 * 升是立刻跳上去（快升慢落），眼睛看着才有"跟着音乐跳"的感觉。 */
#define LED_DECAY_PER_FRAME 3

#define LED_FRAME_MS        33      /* 目标刷新周期，效果帧都按它配速 */
#define LED_TEST_STEP_MS    100     /* 灯珠排列测试每 100ms 前进一颗 */

/* 音频环形缓冲。44.1kHz 下 4096 个单声道样本 ≈ 93ms，够放下一帧还有富余 */
#define LED_RING_SAMPLES    4096

/* 灯板按列顺序串接：每列都从底部向上编号，列与列之间不反向。 */
#define LED_SERPENTINE      0
#define LED_ROW0_AT_TOP     0       /* 第一列从底部开始 */

/*---------------------------------------------------------------------------
 * 状态
 *-------------------------------------------------------------------------*/
static rmt_channel_handle_t s_chan;
static rmt_encoder_handle_t s_encoder;
static TaskHandle_t         s_task;
static volatile bool        s_stop;

static volatile uint8_t s_effect = APP_LED_EFFECT_MANUAL;
static volatile uint8_t s_hue    = 160;     /* 默认偏青蓝，不刺眼 */
static volatile uint8_t s_sat    = 255;
static volatile uint8_t s_bright = 40;      /* 见 LED_MAX_MA 的说明 */
static volatile uint8_t s_test_position;

static uint8_t s_gamma[256];
static uint8_t s_levels[LED_BANDS];         /* 界面读的快照，0~8 */

/* 频谱：环形缓冲（解码任务写、效果任务读） */
static int16_t          s_ring[LED_RING_SAMPLES];
static volatile size_t  s_ring_head;
static volatile size_t  s_ring_tail;
static volatile uint32_t s_ring_rate;
static volatile uint32_t s_ring_drop;        /* 丢了几个样本，调试用 */

/* FFT 的工作区。放静态而不是任务栈上：两个 4KB 的数组会把 4096 的栈撑爆 */
static float s_re[LED_FFT_N];
static float s_im[LED_FFT_N];
static float s_tw_re[LED_FFT_N / 2];
static float s_tw_im[LED_FFT_N / 2];
static uint16_t s_brev[LED_FFT_N];
static int s_band_lo[LED_BANDS];
static int s_band_hi[LED_BANDS];
static uint32_t s_band_rate;                 /* 频段表是按哪个采样率算的 */

/*===========================================================================
 * WS2812：RMT 编码
 *=========================================================================*/
static esp_err_t strip_init(void)
{
    /* 内存块从大到小退着试。太大的请求拿不到连续块时 rmt_new_tx_channel
     * 会失败，而"用多少 RMT 内存"只影响中断频率，不影响能不能发 */
    esp_err_t err = ESP_FAIL;
    for (size_t mb = LED_MEM_BLOCK_MAX; mb >= LED_MEM_BLOCK_MIN; mb -= LED_MEM_BLOCK_STEP) {
        const rmt_tx_channel_config_t cfg = {
            .gpio_num    = APP_MODULE_GPIO_A,
            .clk_src     = RMT_CLK_SRC_DEFAULT,
            .resolution_hz = LED_RMT_RES_HZ,
            .mem_block_symbols = mb,
            .trans_queue_depth = 4,
            .flags = { .invert_out = false, .with_dma = false },
        };
        err = rmt_new_tx_channel(&cfg, &s_chan);
        if (err == ESP_OK) {
            ESP_LOGI(TAG, "RMT 内存块 %u 字", (unsigned)mb);
            break;
        }
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "建 RMT 通道失败: %s", esp_err_to_name(err));
        return err;
    }

    const rmt_bytes_encoder_config_t enc = {
        /* 一个 bit 一个符号：前半段是高电平（数据位靠高电平的长短表示） */
        .bit0 = { .level0 = 1, .duration0 = 4, .level1 = 0, .duration1 = 9 },
        .bit1 = { .level0 = 1, .duration0 = 8, .level1 = 0, .duration1 = 5 },
        .flags = { .msb_first = 1 },    /* WS2812 是高位在前 */
    };
    err = rmt_new_bytes_encoder(&enc, &s_encoder);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "建编码器失败: %s", esp_err_to_name(err));
        rmt_del_channel(s_chan);
        s_chan = NULL;
        return err;
    }

    err = rmt_enable(s_chan);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "RMT enable 失败: %s", esp_err_to_name(err));
        rmt_del_encoder(s_encoder);
        rmt_del_channel(s_chan);
        s_encoder = NULL;
        s_chan = NULL;
        return err;
    }
    return ESP_OK;
}

static void strip_deinit(void)
{
    if (s_chan != NULL) {
        rmt_disable(s_chan);
    }
    if (s_encoder != NULL) {
        rmt_del_encoder(s_encoder);
        s_encoder = NULL;
    }
    if (s_chan != NULL) {
        rmt_del_channel(s_chan);
        s_chan = NULL;
    }
}

/* 把一整帧发出去。**阻塞到发完**（60 颗约 1.9ms），所以只许在效果任务里调 */
static esp_err_t strip_send(const uint8_t *rgb, size_t count)
{
    const rmt_transmit_config_t cfg = { .loop_count = 0 };
    esp_err_t err = rmt_transmit(s_chan, s_encoder, rgb, count * 3, &cfg);
    if (err != ESP_OK) {
        return err;
    }
    return rmt_tx_wait_all_done(s_chan, 100);
}

/*===========================================================================
 * 颜色
 *=========================================================================*/
static void gamma_init(void)
{
    /* 不做 gamma 的话滑条低半段像坏的、色相过渡也难看。这是 FastLED 那种
     * CHSV 里隐含的一步，led_strip 没有，自己来 */
    for (int i = 0; i < 256; i++) {
        s_gamma[i] = (uint8_t)(powf((float)i / 255.0f, 2.2f) * 255.0f + 0.5f);
    }
}

/* h/s/v 都是 0~255（h 是色轮角度，0=红 85=绿 170=蓝） */
static void hsv2rgb(uint8_t h, uint8_t s, uint8_t v,
                    uint8_t *r, uint8_t *g, uint8_t *b)
{
    if (s == 0) {
        *r = *g = *b = v;
        return;
    }
    const uint8_t region = h / 43;
    const uint8_t rem    = (uint8_t)((h - region * 43) * 6);
    const uint8_t p = (uint8_t)(((int)v * (255 - s)) >> 8);
    const uint8_t q = (uint8_t)(((int)v * (255 - (((int)s * rem) >> 8))) >> 8);
    const uint8_t t = (uint8_t)(((int)v * (255 - (((int)s * (255 - rem)) >> 8))) >> 8);

    switch (region) {
    case 0:  *r = v; *g = t; *b = p; break;
    case 1:  *r = q; *g = v; *b = p; break;
    case 2:  *r = p; *g = v; *b = t; break;
    case 3:  *r = p; *g = q; *b = v; break;
    case 4:  *r = t; *g = p; *b = v; break;
    default: *r = v; *g = p; *b = q; break;
    }
}

/*
 * 整帧限流：算出预计电流，超了就整帧同比例缩。
 * **必须整帧一起缩**，逐颗裁会把颜色改掉（红被裁得比绿多，画面就偏了）。
 * 返回值调过一次，缩放系数算一次就够。
 */
static void apply_current_cap(uint8_t *rgb, size_t count)
{
    uint32_t sum = 0;
    for (size_t i = 0; i < count * 3; i++) {
        sum += rgb[i];
    }
    const uint32_t est_ma = (uint32_t)count * LED_MA_IDLE +
                            (uint32_t)((uint64_t)sum * LED_MA_PER_CH / 255);
    if (est_ma <= LED_MAX_MA) {
        return;
    }
    const uint32_t scale = (uint32_t)((uint64_t)LED_MAX_MA * 256 / est_ma);
    for (size_t i = 0; i < count * 3; i++) {
        rgb[i] = (uint8_t)((rgb[i] * scale) >> 8);
    }
}

/*===========================================================================
 * 频谱：PCM 环形缓冲
 *=========================================================================*/
/*
 * app_audio 的解码任务回调。**跑在解码任务里，必须立刻返回**，所以这里
 * 只做 L/R 合并 + 存环形缓冲，一点分析都不做。
 *
 * 存的是单声道：L/R 平均。立体声两个都送进 FFT 要多一倍的算力，而 8 根柱子
 * 分辨不出声道差异，没有意义。
 */
static void on_pcm(const int16_t *pcm, size_t frames, uint32_t rate_hz, void *user)
{
    (void)user;
    s_ring_rate = rate_hz;

    for (size_t i = 0; i < frames; i++) {
        const int32_t mono = ((int32_t)pcm[i * 2] + pcm[i * 2 + 1]) / 2;
        const size_t next = (s_ring_head + 1) % LED_RING_SAMPLES;
        if (next == s_ring_tail) {
            /* 满了：**丢新数据，不动 tail**。动 tail 是读端的活，两端各碰一个
             * 指针才不会打架；而"数据过期"由读端自己跳过去解决 */
            s_ring_drop++;
            return;
        }
        s_ring[s_ring_head] = (int16_t)mono;
        s_ring_head = next;
    }
}

/*
 * 取 n 个样本。不够就补零（说明音乐停/卡了），此时**不推进 tail**，
 * 这样音乐一恢复就能接着上。
 */
static void ring_read(int16_t *out, size_t n)
{
    size_t avail = (s_ring_head + LED_RING_SAMPLES - s_ring_tail) % LED_RING_SAMPLES;

    /* 积压超过两帧就跳到最新的那一帧，别让画面跟在音乐后面跑。
     * 显示类的东西宁可丢一段也不能有延迟 */
    if (avail > n * 2) {
        s_ring_tail = (s_ring_head + LED_RING_SAMPLES - n) % LED_RING_SAMPLES;
        avail = n;
    }

    const size_t take = (avail < n) ? avail : n;
    for (size_t i = 0; i < take; i++) {
        out[i] = s_ring[s_ring_tail];
        s_ring_tail = (s_ring_tail + 1) % LED_RING_SAMPLES;
    }
    for (size_t i = take; i < n; i++) {
        out[i] = 0;
    }
}

/*===========================================================================
 * 频谱：FFT 与频段
 *=========================================================================*/
static void fft_init(void)
{
    for (int i = 0; i < LED_FFT_N; i++) {
        /* 位反转表 */
        uint16_t r = 0;
        for (int b = 0; b < 9; b++) {          /* 2^9 = 512 */
            r = (uint16_t)((r << 1) | ((i >> b) & 1));
        }
        s_brev[i] = r;
    }
    for (int k = 0; k < LED_FFT_N / 2; k++) {
        const float ang = -2.0f * (float)M_PI * k / LED_FFT_N;
        s_tw_re[k] = cosf(ang);
        s_tw_im[k] = sinf(ang);
    }
}

/* 原地 radix-2 复数 FFT（实数输入时 im[] 进来是全 0）。
 * N=512 约 23k 次浮点乘加，240MHz 下单核 0.3~0.5ms——一帧 33ms 里占 1.5%，
 * 没有任何优化的必要 */
static void fft_run(float *re, float *im)
{
    for (int i = 0; i < LED_FFT_N; i++) {
        const int j = s_brev[i];
        if (j > i) {
            float t = re[i]; re[i] = re[j]; re[j] = t;
            t = im[i]; im[i] = im[j]; im[j] = t;
        }
    }
    for (int len = 2; len <= LED_FFT_N; len <<= 1) {
        const int half = len >> 1;
        const int step = LED_FFT_N / len;
        for (int i = 0; i < LED_FFT_N; i += len) {
            for (int k = 0; k < half; k++) {
                const int tw = k * step;
                const float wr = s_tw_re[tw], wi = s_tw_im[tw];
                const int a = i + k, b = a + half;
                const float tr = re[b] * wr - im[b] * wi;
                const float ti = re[b] * wi + im[b] * wr;
                re[b] = re[a] - tr;
                im[b] = im[a] - ti;
                re[a] += tr;
                im[a] += ti;
            }
        }
    }
}

/*
 * 按当前采样率重建频段表。**不重采样**：卡里的歌 16k/22.05k/44.1k/48k 都有，
 * 重采样要多一层滤波和一堆边界情况，而频段本来就是按 Hz 定的，binHz 跟着
 * 采样率变一下就完了。
 */
static void bands_rebuild(uint32_t rate)
{
    const float bin_hz = (float)rate / LED_FFT_N;
    /* 上沿别顶到奈奎斯特：16kHz 时 6400Hz 已经在 0.8*Nyquist 了 */
    float f_hi = LED_F_HI;
    if (f_hi > (float)rate * 0.42f) {
        f_hi = (float)rate * 0.42f;
    }
    const int max_bin = LED_FFT_N / 2 - 1;
    const float ratio = f_hi / LED_F_LO;

    for (int c = 0; c < LED_BANDS; c++) {
        const float f_low = LED_F_LO * powf(ratio, (float)c / LED_BANDS);
        const float f_up  = LED_F_LO * powf(ratio, (float)(c + 1) / LED_BANDS);
        int lo = (int)(f_low / bin_hz);
        int hi = (int)(f_up / bin_hz);
        if (lo < 1) lo = 1;                     /* 0 号 bin 是直流，丢掉 */
        if (hi > max_bin) hi = max_bin;
        if (hi < lo) hi = lo;
        s_band_lo[c] = lo;
        s_band_hi[c] = hi;
    }
    s_band_rate = rate;
    ESP_LOGI(TAG, "频段表按 %u Hz 重建（bin = %.1f Hz，上沿 %.0f Hz）",
             (unsigned)rate, bin_hz, f_hi);
}

/* 一帧 PCM → 每个频段一根柱子的目标高度（0~LED_ROWS） */
static void spectrum_analyze(const int16_t *mono, uint8_t *out)
{
    /* 去直流 + 归一化 + 汉宁窗。去直流不能省：MP3 解码出来的可能有直流偏置，
     * 不去的话 0 号 bin（也就是低频段的邻居）会被顶起来，低音柱子一直是满的 */
    float mean = 0.0f;
    for (int i = 0; i < LED_FFT_N; i++) {
        mean += (float)mono[i];
    }
    mean /= LED_FFT_N;

    for (int i = 0; i < LED_FFT_N; i++) {
        const float w = 0.5f - 0.5f * cosf(2.0f * (float)M_PI * i / (LED_FFT_N - 1));
        s_re[i] = ((float)mono[i] - mean) / 32768.0f * w;
        s_im[i] = 0.0f;
    }

    fft_run(s_re, s_im);

    /* 只取前一半（实数信号的频谱是对称的） */
    const int half = LED_FFT_N / 2;
    for (int i = 0; i < half; i++) {
        s_re[i] = sqrtf(s_re[i] * s_re[i] + s_im[i] * s_im[i]);
    }

    const float log_den = log10f(1.0f + LED_LOG_FULL_SCALE);
    for (int c = 0; c < LED_BANDS; c++) {
        const int lo = s_band_lo[c], hi = s_band_hi[c];
        float sum = 0.0f;
        for (int b = lo; b <= hi; b++) {
            sum += s_re[b];
        }
        /* 段内取平均而不是求和：高频段比低频段宽得多，求和的话永远是高频赢 */
        const float mag = sum / (float)(hi - lo + 1) * LED_SENSITIVITY;
        float norm = log10f(1.0f + mag) / log_den;
        if (norm < 0.0f) norm = 0.0f;
        if (norm > 1.0f) norm = 1.0f;
        out[c] = (uint8_t)(norm * LED_ROWS + 0.5f);
    }
}

/*===========================================================================
 * 渲染
 *=========================================================================*/
static int xy_to_index(int col, int row)
{
    const int r = LED_ROW0_AT_TOP ? row : (LED_ROWS - 1 - row);
    int mapped_row = r;
    if (LED_SERPENTINE && (col & 1)) {
        mapped_row = LED_ROWS - 1 - r;
    }
    return col * LED_ROWS + mapped_row;
}

static void render_manual(uint8_t *rgb)
{
    uint8_t r, g, b;
    hsv2rgb(s_hue, s_sat, 255, &r, &g, &b);
    for (int i = 0; i < APP_LED_COUNT; i++) {
        rgb[i * 3 + 0] = r;
        rgb[i * 3 + 1] = g;
        rgb[i * 3 + 2] = b;
    }
}

static void render_spectrum(uint8_t *rgb)
{
    memset(rgb, 0, APP_LED_COUNT * 3);
    for (int c = 0; c < LED_BANDS; c++) {
        const int h = s_levels[c];
        for (int level = 0; level < h && level < LED_ROWS; level++) {
            const int row = LED_ROWS - 1 - level;
            /* 低的那几行偏青、顶上的偏红，看着像条音量表 */
            uint8_t r, g, b;
            hsv2rgb((uint8_t)(160 - level * 22), 255, 255, &r, &g, &b);
            const int idx = xy_to_index(c, row);
            rgb[idx * 3 + 0] = r;
            rgb[idx * 3 + 1] = g;
            rgb[idx * 3 + 2] = b;
        }
    }
}

static void render_test(uint8_t *rgb)
{
    memset(rgb, 0, APP_LED_COUNT * 3);
    uint8_t r, g, b;
    hsv2rgb(s_hue, s_sat, 255, &r, &g, &b);
    const int offset = s_test_position * 3;
    rgb[offset + 0] = r;
    rgb[offset + 1] = g;
    rgb[offset + 2] = b;
}

/* 亮度 → gamma → 限流 → 发出去。三个效果的收尾都一样，统一在这里 */
static esp_err_t push_frame(uint8_t *rgb)
{
    const int scale = s_bright;
    for (int i = 0; i < APP_LED_COUNT * 3; i++) {
        const uint8_t v = s_gamma[((int)rgb[i] * scale) / 100];
        rgb[i] = v;
    }
    apply_current_cap(rgb, APP_LED_COUNT);
    esp_err_t err = strip_send(rgb, APP_LED_COUNT);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "发送失败: %s", esp_err_to_name(err));
    }
    return err;
}

/*===========================================================================
 * 效果任务
 *=========================================================================*/
static void led_task(void *arg)
{
    (void)arg;

    uint8_t  rgb[APP_LED_COUNT * 3];
    int16_t  mono[LED_FFT_N];
    uint8_t  target[LED_BANDS];
    TickType_t last = xTaskGetTickCount();
    TickType_t test_last_step = 0;

    while (!s_stop) {
        const app_led_effect_t effect = (app_led_effect_t)s_effect;
        if (effect == APP_LED_EFFECT_SPECTRUM) {
            test_last_step = 0;
            ring_read(mono, LED_FFT_N);

            /* 采样率和频段表对不上就重建。放在这里而不是回调里：回调在
             * 解码任务上，重建要算 8 次 powf，不该占人家的时间 */
            const uint32_t rate = s_ring_rate;
            if (rate > 0 && rate != s_band_rate) {
                bands_rebuild(rate);
            }

            if (s_band_rate == 0) {
                /* 一次都没收到过 PCM（音乐没在放）：柱子全灭，别显示上一帧
                 * 的残留数字——那看着像"数据还在来" */
                memset(s_levels, 0, sizeof(s_levels));
            } else {
                spectrum_analyze(mono, target);
                for (int c = 0; c < LED_BANDS; c++) {
                    if (target[c] >= s_levels[c]) {
                        s_levels[c] = target[c];            /* 快升 */
                    } else if (s_levels[c] > LED_DECAY_PER_FRAME) {
                        s_levels[c] -= LED_DECAY_PER_FRAME; /* 慢落 */
                    } else {
                        s_levels[c] = 0;
                    }
                }
            }
            render_spectrum(rgb);
        } else if (effect == APP_LED_EFFECT_TEST) {
            const TickType_t now = xTaskGetTickCount();
            if (test_last_step == 0) {
                test_last_step = now;
            } else if ((now - test_last_step) >= pdMS_TO_TICKS(LED_TEST_STEP_MS)) {
                s_test_position = (s_test_position + 1) % APP_LED_COUNT;
                test_last_step = now;
            }
            render_test(rgb);
        } else {
            test_last_step = 0;
            render_manual(rgb);
        }

        push_frame(rgb);

        /* 效果帧都按 33ms 配速。频谱这边**不能让 PCM 的到达速度决定刷新率**：
         * 44.1kHz 下一帧 512 点只有 11.6ms，不拦着就会以 86fps 去刷 RMT，
         * 白烧 16% 的核 */
        vTaskDelayUntil(&last, pdMS_TO_TICKS(LED_FRAME_MS));
    }

    s_task = NULL;
    vTaskDelete(NULL);
}

/*===========================================================================
 * 对外接口
 *=========================================================================*/
esp_err_t app_led2812_enable(void)
{
    if (app_module_selected() != APP_MODULE_LED2812) {
        /* 没轮到我们。模块口那两根脚现在借给别人了，不能伸手 */
        return ESP_ERR_INVALID_STATE;
    }
    if (s_task != NULL) {
        return ESP_OK;
    }

    gamma_init();
    fft_init();

    esp_err_t err = strip_init();
    if (err != ESP_OK) {
        return err;
    }

    /* 一进来就是黑的，等第一帧渲染出来才亮 */
    memset(s_ring, 0, sizeof(s_ring));
    s_ring_head = s_ring_tail = 0;
    s_ring_rate = 0;
    s_ring_drop = 0;
    s_band_rate = 0;
    memset(s_levels, 0, sizeof(s_levels));

    /* 挂上 PCM 抽头。音乐没播时它一个字节都不会来（回调只在解码循环里调） */
    app_audio_set_pcm_cb(on_pcm, NULL);

    s_stop = false;
    if (xTaskCreatePinnedToCore(led_task, "led2812", 4096, NULL, 3, &s_task, 1) != pdPASS) {
        s_task = NULL;
        app_audio_set_pcm_cb(NULL, NULL);
        strip_deinit();
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG, "灯板已启动（%d 颗，GPIO%d）", APP_LED_COUNT, APP_MODULE_GPIO_A);
    return ESP_OK;
}

void app_led2812_disable(void)
{
    if (s_task == NULL) {
        return;
    }

    s_stop = true;
    for (int i = 0; i < 25 && s_task != NULL; i++) {
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    if (s_task != NULL) {
        ESP_LOGE(TAG, "效果任务没退出来，RMT 保持原样");
        return;
    }

    /* 先摘抽头：摘完之后解码任务不会再往环形缓冲里写 */
    app_audio_set_pcm_cb(NULL, NULL);

    /* **熄灯必须在拆通道之前**：灯板自己带电就一直保持着最后一个画面，
     * 不熄的话关掉模块以后它还亮着，看着像没关掉。发一帧全黑再拆 */
    uint8_t black[APP_LED_COUNT * 3] = { 0 };
    strip_send(black, APP_LED_COUNT);

    strip_deinit();
    app_module_port_idle();

    if (s_ring_drop > 0) {
        ESP_LOGI(TAG, "本次运行丢弃了 %u 个样本（环形缓冲满）", (unsigned)s_ring_drop);
    }
    ESP_LOGI(TAG, "灯板已停止");
}

bool app_led2812_is_active(void)
{
    return s_task != NULL;
}

bool app_led2812_needs_audio(void)
{
    return s_task != NULL && s_effect == APP_LED_EFFECT_SPECTRUM;
}

esp_err_t app_led2812_set_effect(app_led_effect_t effect)
{
    if (effect != APP_LED_EFFECT_MANUAL &&
        effect != APP_LED_EFFECT_SPECTRUM &&
        effect != APP_LED_EFFECT_TEST) {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_task == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    if (effect == APP_LED_EFFECT_TEST && s_effect != APP_LED_EFFECT_TEST) {
        s_test_position = 0;
    }
    s_effect = (uint8_t)effect;
    if (effect == APP_LED_EFFECT_MANUAL) {
        /* 切回手动时把柱子清掉，否则残留的高度会一直挂在快照里 */
        memset(s_levels, 0, sizeof(s_levels));
    }
    const char *name = effect == APP_LED_EFFECT_MANUAL ? "手动" :
                       effect == APP_LED_EFFECT_SPECTRUM ? "频谱" : "灯珠测试";
    ESP_LOGI(TAG, "效果切到 %s", name);
    return ESP_OK;
}

app_led_effect_t app_led2812_effect(void)
{
    return (app_led_effect_t)s_effect;
}

uint8_t app_led2812_test_position(void)
{
    return s_test_position + 1;
}

esp_err_t app_led2812_set_hsv(uint8_t h, uint8_t s)
{
    s_hue = h;
    s_sat = s;
    return ESP_OK;
}

esp_err_t app_led2812_set_brightness(uint8_t percent)
{
    s_bright = (percent > 100) ? 100 : percent;
    return ESP_OK;
}

uint8_t app_led2812_brightness(void)
{
    return s_bright;
}

void app_led2812_get_levels(uint8_t *out, int max_bands)
{
    if (out == NULL || max_bands <= 0) {
        return;
    }
    const int n = (max_bands < LED_BANDS) ? max_bands : LED_BANDS;
    /* 快照读，最坏旧一帧。柱子画歪一格没人看得出来，不值得为它上锁 */
    for (int i = 0; i < n; i++) {
        out[i] = s_levels[i];
    }
}
