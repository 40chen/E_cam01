/*
 * app_sound.c — 一次性音效播放
 *
 * 就一条路径：把 flash 里的 PCM 分块喂进 I2S，喂完等 DMA 排空，关功放。
 * 播放前按素材自己的采样率重配一次 I2S —— 上一次出声可能是音乐页里那首
 * 44.1kHz 的歌，不切就会用错时钟放（变调、时长不对）。
 */
#include "app_sound.h"

#include <string.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "app_audio.h"
#include "lckfb_bsp.h"
#include "shutter_sound.h"

static const char *TAG = "sound";

/*
 * 每次喂给 I2S 的块大小。4096 字节 = 1024 帧立体声 = 16kHz 下 64ms。
 *
 * 为什么不分块不行：I2S 的 DMA 读不了 flash，必须先把数据拷进可 DMA 的内存。
 * 整段拷（最长的那版 15988 字节）看着也行，但这里要留余量——实测相机 +
 * WiFi + LVGL 全跑着的时候，一次性要 96KB 的 DMA 内存会直接失败。4KB 是
 * 稳拿的量，代价只是多几次 memcpy。
 */
#define SOUND_CHUNK_BYTES 4096

/*
 * 写完到关功放之间等多久。I2S 的 DMA 是 6 × 240 帧（i2s_chan 默认值），
 * 16kHz 下是 90ms。取 120ms 留 30ms 余量。
 *
 * 这个常数是按 16kHz 定的：采样率更低的话 DMA 排空更慢，要跟着调大。
 * 现在所有素材都是 16kHz，所以够用。少了这一段会听见尾巴被切掉（最后
 * 那点余音"嘎"一声没了），多了就是功放白开一会儿。
 */
#define SOUND_DRAIN_MS 120

esp_err_t app_sound_play_pcm(const uint8_t *pcm, uint32_t bytes, uint32_t rate)
{
    if (pcm == NULL || bytes == 0 || rate == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    /* 用户可能压根没进过取景页，音频从没初始化过。bsp_audio_init 可重复调用 */
    esp_err_t err = bsp_audio_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "audio init failed: %s", esp_err_to_name(err));
        return err;
    }

    /*
     * **把当前音量应用一遍。** 音量是全局的（音乐页和设置页那两条滑条写的是
     * app_audio 里同一个值），而 bsp_audio_init() 在"头一回初始化"时是按 BSP
     * 自己的默认值（BSP_AUDIO_DEFAULT_VOLUME = 93）打开 codec 的，跟 app_audio
     * 存着的那个（默认 50）不是一个数。
     *
     * 不补这一下的话：开机后直接去取景页按快门，快门声按 93 出（比音乐还响），
     * 而设置页那条滑条上写着 50——两边对不上，而且"调了音量没同步到快门声"
     * 这个现象只在动过滑条之后才消失，很难理解。
     *
     * 取出来再原样设回去：**目的不是改值，是让 app_audio 把它写进 ES8311**
     * （setter 里那句 apply_volume）。app_audio 那边没有单独的"应用当前值"
     * 接口，加一个不如这么写一句。
     */
    int vol = 0;
    if (app_audio_get_volume(&vol) == ESP_OK) {
        app_audio_set_volume(vol);
    }

    err = bsp_audio_set_fs(rate, 16, 2);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "set fs %u Hz failed: %s", (unsigned)rate, esp_err_to_name(err));
        return err;
    }

    uint8_t *buf = heap_caps_malloc(SOUND_CHUNK_BYTES, MALLOC_CAP_DMA);
    if (buf == NULL) {
        ESP_LOGE(TAG, "no DMA mem for %d byte chunk", SOUND_CHUNK_BYTES);
        return ESP_ERR_NO_MEM;
    }

    bsp_pa_en(1);

    uint32_t done = 0;
    while (done < bytes) {
        uint32_t n = bytes - done;
        if (n > SOUND_CHUNK_BYTES) {
            n = SOUND_CHUNK_BYTES;
        }
        memcpy(buf, pcm + done, n);

        size_t written = 0;
        err = bsp_audio_write(buf, n, &written, 1000);
        done += written;
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "i2s write stopped at %u/%u: %s",
                     (unsigned)done, (unsigned)bytes, esp_err_to_name(err));
            break;
        }
    }

    /* 写完 DMA 里还剩一圈没放完，等它出来再关功放 */
    vTaskDelay(pdMS_TO_TICKS(SOUND_DRAIN_MS));
    bsp_pa_en(0);
    free(buf);

    /* 这一行是排查"没声音"最有用的输出：真实写进 I2S 的字节数。
     * 对得上但还是没声音 → 问题在 I2S 之后（ES8311 / 功放）；对不上 → 在写入。 */
    ESP_LOGI(TAG, "played %u/%u bytes @ %u Hz", (unsigned)done, (unsigned)bytes,
             (unsigned)rate);
    return (done >= bytes) ? ESP_OK : ESP_FAIL;
}

/*---------------------------------------------------------------------------
 * 快门声
 *-------------------------------------------------------------------------*/
static bool s_shutter_on = true;

void app_sound_set_shutter_enabled(bool on)
{
    s_shutter_on = on;
}

bool app_sound_shutter_enabled(void)
{
    return s_shutter_on;
}

static esp_err_t play_variant0(void)
{
    /* 表里只有一个音色（见 shutter_sound.h），所以这里直接取第 0 项 */
    const shutter_sound_t *s = &shutter_sound_variants[0];
    return app_sound_play_pcm(s->pcm, s->bytes, s->rate);
}

esp_err_t app_sound_play_shutter(void)
{
    if (!s_shutter_on) {
        return ESP_OK;
    }
    return play_variant0();
}

esp_err_t app_sound_play_shutter_preview(void)
{
    return play_variant0();
}
