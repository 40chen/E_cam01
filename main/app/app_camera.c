#include "app_camera.h"

#include <stdio.h>
#include <string.h>
#include <time.h>

#include "esp_camera.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "app_storage.h"
#include "lckfb_bsp.h"

static const char *TAG = "camera";

/*
 * 15fps。这个数字同时压着两件事：
 *
 * 1. 输入延迟。预览每帧都让整屏失效，LVGL 跟着重绘；这个任务每帧还要抢一次
 *    LVGL 锁（见 on_frame），抢不到的那一轮 esp_lvgl_port 主循环整轮跳过——
 *    连触摸采样定时器都不跑。触摸是纯轮询的（板子没接触摸中断脚），采样间隔
 *    被拉长且不规律时，快速轻点整段落在空隙里就不会被记录。
 *
 * 2. 撕裂。板子没接 TE 信号，面板自己在逐行扫描，而我们往 GRAM 里写的时候
 *    没有任何同步手段——刷新的那 15ms（153KB @ 80MHz）里面板扫到哪儿哪儿就是
 *    接缝。写多久由 SPI 速率定死了，所以只能减少写入频率。
 *
 * 从 30fps 一路降到 15fps，这两个问题各缓解一半左右。取景会顿一点，
 * 但"按键点了没反应"和"画面撕裂"都比它更影响使用。
 */
#define PREVIEW_INTERVAL_MS   66      /* 15fps */
#define JPEG_QUALITY          90
/* 时间未同步时用序号命名，扫描目录取当前最大值 +1 */
#define PHOTO_INDEX_UNSYNCED  1

/* 预览尺寸 = 屏幕尺寸。传感器跑 VGA，这一路自己缩一半（见 downscale_half） */
#define PREVIEW_W             BSP_LCD_H_RES
#define PREVIEW_H             BSP_LCD_V_RES

static SemaphoreHandle_t s_lock = NULL;      /* 串行化取帧与抓拍 */
static TaskHandle_t      s_task = NULL;
static volatile bool     s_running = false;

static app_camera_frame_cb_t s_frame_cb = NULL;
static void                 *s_frame_user = NULL;

/*
 * 预览用的双缓冲。传感器给的是 VGA，缩到 QVGA 之后交给界面。
 * 要两块交替：lv_img_set_src() 只是换指针，真正的绘制发生在 LVGL 任务里，
 * 用一块的话下一帧会把正在画的那份覆掉，画面上就是撕裂。
 */
static uint16_t *s_preview_buf[2] = { NULL, NULL };
static int       s_preview_idx = 0;          /* 当前交给界面的那块 */

/*---------------------------------------------------------------------------
 * VGA -> QVGA：2x2 抽取
 *
 * 为什么不让传感器自己输出 QVGA：**cam_hal 会校验收到帧的长度和配置的
 * fb_size 是否相等**（driver/cam_hal.c 的 ISR 里），不相等就不把帧入队，
 * 而且那块缓冲还会被永久标记为占用。几帧之后所有缓冲全卡死，取帧一直超时，
 * 最后中断看门狗复位。所以传感器只能全程跑一种尺寸，预览这里自己缩。
 *
 * 每 2x2 取左上角那一个点。不做平均：一来省掉 RGB565 拆通道的运算，二来
 * GC0308 自己的 QVGA 模式也是抽样出来的，观感一致。
 *-------------------------------------------------------------------------*/
static void downscale_half(const uint16_t *src, int src_w,
                           uint16_t *dst, int dst_w, int dst_h)
{
    for (int y = 0; y < dst_h; y++) {
        const uint16_t *row = src + (size_t)(y * 2) * src_w;
        uint16_t *out = dst + (size_t)y * dst_w;
        for (int x = 0; x < dst_w; x++) {
            out[x] = row[x * 2];
        }
    }
}

/*---------------------------------------------------------------------------
 * 文件名
 *-------------------------------------------------------------------------*/
static bool parse_unsynced_index(const char *name, unsigned *out)
{
    /* 期望形如 IMG_00042.jpg */
    if (strncmp(name, "IMG_", 4) != 0) {
        return false;
    }
    unsigned v = 0;
    const char *p = name + 4;
    if (*p < '0' || *p > '9') {
        return false;
    }
    while (*p >= '0' && *p <= '9') {
        v = v * 10 + (unsigned)(*p - '0');
        p++;
    }
    if (strcmp(p, ".jpg") != 0) {
        return false;
    }
    *out = v;
    return true;
}

static bool scan_max_index_cb(const char *name, void *user)
{
    unsigned *max = (unsigned *)user;
    unsigned v;
    if (parse_unsynced_index(name, &v) && v > *max) {
        *max = v;
    }
    return true;
}

static void build_photo_path(char *out, size_t len)
{
    time_t now = time(NULL);
    struct tm tm_now;
    localtime_r(&now, &tm_now);

    if (tm_now.tm_year + 1900 >= 2020) {
        /* SNTP 已同步，按需求用 IMG_YYYYMMDD_HHMMSS.jpg */
        snprintf(out, len, APP_PHOTO_DIR "/IMG_%04d%02d%02d_%02d%02d%02d.jpg",
                 tm_now.tm_year + 1900, tm_now.tm_mon + 1, tm_now.tm_mday,
                 tm_now.tm_hour, tm_now.tm_min, tm_now.tm_sec);
        return;
    }

    /* 还没对时：所有照片都会落到 1970 年同一秒，必须退化成序号命名，
     * 否则会互相覆盖。 */
    unsigned max = 0;
    app_storage_list(APP_PHOTO_DIR, ".jpg", scan_max_index_cb, &max);
    snprintf(out, len, APP_PHOTO_DIR "/IMG_%05u.jpg", max + PHOTO_INDEX_UNSYNCED);
}

/*---------------------------------------------------------------------------
 * 取帧任务
 *-------------------------------------------------------------------------*/
static void preview_task(void *arg)
{
    /* 这里可以立刻归还摄像头缓冲：缩完就不再需要原始帧了，交给界面的
     * 是 s_preview_buf 里的一份拷贝，与摄像头缓冲无关。
     * （以前是零拷贝直接把摄像头缓冲交出去，欠一帧才能归还——现在不用了） */
    while (s_running) {
        /*
         * 回调**必须读到局部变量再用**。
         *
         * 退出取景页时 app_camera_set_frame_cb(NULL, NULL) 会把它清掉，而
         * 下面 downscale_half 要跑十几毫秒——检查和调用之间隔了这么久，
         * 用户在这期间退出就会让 s_frame_cb 在两步之间变成 NULL，调用一个
         * 空函数指针直接 PC=0 复位（InstrFetchProhibited）。
         *
         * 用局部变量之后，调用的永远是读到那一刻的有效指针。函数本身
         * （on_frame）是静态的、永远有效，它会自己判断界面还在不在。
         */
        app_camera_frame_cb_t cb = s_frame_cb;
        void *user = s_frame_user;
        if (cb == NULL) {
            vTaskDelay(pdMS_TO_TICKS(50));
            continue;
        }

        if (xSemaphoreTake(s_lock, pdMS_TO_TICKS(1000)) != pdTRUE) {
            continue;
        }
        camera_fb_t *fb = esp_camera_fb_get();
        if (fb != NULL) {
            /*
             * 传感器必须正好是预览的两倍，否则 downscale_half 会读越界
             * 并写穿缓冲。这是配置错误不是运行期状况，但仍然**只跳过这一帧**、
             * 不动 s_frame_cb——取帧任务擅自把界面的回调清掉是个坏设计，
             * 界面那边会以为摄像头还活着。
             */
            if (fb->width != PREVIEW_W * 2 || fb->height != PREVIEW_H * 2) {
                ESP_LOGE(TAG, "unexpected frame %ux%u, preview needs %ux%u (frame skipped)",
                         (unsigned)fb->width, (unsigned)fb->height,
                         PREVIEW_W * 2, PREVIEW_H * 2);
            } else {
                /* 写另一块，界面上正在画的那块不能踩 */
                uint16_t *back = s_preview_buf[s_preview_idx ^ 1];
                downscale_half((const uint16_t *)fb->buf, fb->width,
                               back, PREVIEW_W, PREVIEW_H);
                cb((const uint8_t *)back, PREVIEW_W, PREVIEW_H, user);
                s_preview_idx ^= 1;
            }
        }
        if (fb != NULL) {
            esp_camera_fb_return(fb);
        }
        xSemaphoreGive(s_lock);

        vTaskDelay(pdMS_TO_TICKS(PREVIEW_INTERVAL_MS));
    }

    /*
     * 只有 s_task 还是自己时才清空。等超时之后 app_camera_start() 可能已经
     * 建了新任务并把句柄写进 s_task，这时旧任务退出再把它置 NULL，就会让
     * 后面那次 app_camera_stop() 以为没人在跑，直接去拆摄像头。
     */
    if (s_task == xTaskGetCurrentTaskHandle()) {
        s_task = NULL;
    }
    vTaskDelete(NULL);
}

/*---------------------------------------------------------------------------
 * 对外接口
 *-------------------------------------------------------------------------*/
esp_err_t app_camera_start(void)
{
    if (s_running) {
        return ESP_OK;
    }
    /*
     * 上一轮的取帧任务还没退出（app_camera_stop 等超时了）时不能再建一个：
     * 两个任务会同时取帧，而且新句柄写进 s_task 之后旧任务就没人管了——
     * 它退出时还会把新句柄清掉，让下一次 stop 以为没人在跑。
     * 宁可让界面显示"相机不可用"，也不要这种状态。
     */
    if (s_task != NULL) {
        ESP_LOGE(TAG, "previous preview task still running, refusing to start");
        return ESP_ERR_INVALID_STATE;
    }
    if (s_lock == NULL) {
        s_lock = xSemaphoreCreateMutex();
        if (s_lock == NULL) {
            return ESP_ERR_NO_MEM;
        }
    }

    /* 预览缩放用的双缓冲。放 PSRAM：一块 153KB，内部 RAM 放不下 */
    if (s_preview_buf[0] == NULL) {
        const size_t sz = (size_t)PREVIEW_W * PREVIEW_H * sizeof(uint16_t);
        for (int i = 0; i < 2; i++) {
            s_preview_buf[i] = heap_caps_malloc(sz, MALLOC_CAP_SPIRAM);
            if (s_preview_buf[i] == NULL) {
                ESP_LOGE(TAG, "no PSRAM for preview buffer %d", i);
                for (int j = 0; j < 2; j++) {
                    free(s_preview_buf[j]);
                    s_preview_buf[j] = NULL;
                }
                return ESP_ERR_NO_MEM;
            }
        }
    }
    s_preview_idx = 0;

    esp_err_t err = bsp_camera_init();
    if (err != ESP_OK) {
        return err;
    }

    s_running = true;
    /*
     * 优先级 3，**必须低于 LVGL 任务的 4**。
     *
     * 以前是 5（比 UI 高）。那时取帧只是把指针交给 lv_img，几乎不占 CPU，
     * 高优先级无所谓。现在每帧要做 VGA->QVGA 的缩放，那是十几毫秒的纯 CPU
     * 循环，比 UI 高的话每帧都会把 LVGL 抢占一次——而触摸采样就跑在 LVGL
     * 任务里（板子没接触摸中断脚），被抢占期间整段采样丢失，表现就是
     * 取景页"按了没反应"。
     *
     * 降到 3 之后预览变成尽力而为：UI 忙时掉帧，但按键跟手。预览掉几帧
     * 用户几乎看不出来，按键不灵是一眼就能看出来的。
     */
    if (xTaskCreatePinnedToCore(preview_task, "cam_preview", 4096, NULL, 3, &s_task, 1) != pdPASS) {
        s_running = false;
        bsp_camera_deinit();
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

esp_err_t app_camera_stop(void)
{
    if (!s_running) {
        return ESP_OK;
    }
    s_running = false;
    s_frame_cb = NULL;

    /* 等取帧任务自己退出，避免它还在用摄像头时被 deinit */
    for (int i = 0; i < 50 && s_task != NULL; i++) {
        vTaskDelay(pdMS_TO_TICKS(20));
    }

    bsp_camera_deinit();

    /*
     * 预览缓冲**不释放**，一次分配一直留着（共 300KB PSRAM）。
     *
     * 曾经在这里 free 过，结果是进出取景页几次之后整机复位：上面的等待只有
     * 1 秒，而取帧任务可能正卡在 esp_camera_fb_get() 里（那个内部超时是
     * 4 秒），等不到它就把它脚下的 153KB 缓冲释放了。任务接着往已释放的堆块
     * 上写缩放结果，堆被踩烂，下一次 xTaskCreate 拿到的任务入口指针成了垃圾，
     * 一跑就是 InstrFetchProhibited（PC=0）。
     *
     * 300KB 换掉这一整类堆损坏，划算。
     *
     * 另外：等超时之后摄像头仍会被 deinit，这本身是安全的——
     * esp_camera_fb_get() 开头就是 if (s_state == NULL) return NULL;
     */
    ESP_LOGI(TAG, "camera stopped");
    return ESP_OK;
}

bool app_camera_is_running(void)
{
    return s_running;
}

void app_camera_set_frame_cb(app_camera_frame_cb_t cb, void *user)
{
    s_frame_user = user;
    s_frame_cb = cb;
}

esp_err_t app_camera_capture(char *out_path, size_t path_len)
{
    if (!s_running) {
        return ESP_ERR_INVALID_STATE;
    }
    if (!app_storage_ready()) {
        ESP_LOGW(TAG, "capture refused: no storage");
        return ESP_ERR_INVALID_STATE;
    }
    if (s_lock == NULL || xSemaphoreTake(s_lock, pdMS_TO_TICKS(2000)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }

    esp_err_t ret = ESP_FAIL;

    /* 传感器全程跑 VGA，直接取一帧就是 640x480，不需要切尺寸 */
    camera_fb_t *fb = esp_camera_fb_get();
    if (fb == NULL) {
        ESP_LOGE(TAG, "capture: no frame");
        goto done;
    }

    uint8_t *jpg = NULL;
    size_t jpg_len = 0;
    /* GC0308 不输出 JPEG，这里用 esp32-camera 内置的软件编码器
     * （frame2jpg -> esp_jpeg），把 RGB565 帧转成 JPEG。 */
    if (!frame2jpg(fb, JPEG_QUALITY, &jpg, &jpg_len) || jpg == NULL) {
        ESP_LOGE(TAG, "capture: jpeg encode failed");
        esp_camera_fb_return(fb);
        goto done;
    }
    esp_camera_fb_return(fb);

    char path[96];
    build_photo_path(path, sizeof(path));

    FILE *f = fopen(path, "wb");
    if (f == NULL) {
        ESP_LOGE(TAG, "capture: cannot open %s", path);
        free(jpg);
        goto done;
    }
    size_t written = fwrite(jpg, 1, jpg_len, f);
    fclose(f);
    free(jpg);

    if (written != jpg_len) {
        ESP_LOGE(TAG, "capture: short write %u/%u", (unsigned)written, (unsigned)jpg_len);
        remove(path);
        goto done;
    }

    ESP_LOGI(TAG, "saved %s (%u bytes)", path, (unsigned)jpg_len);
    if (out_path != NULL) {
        strlcpy(out_path, path, path_len);
    }
    ret = ESP_OK;

done:
    xSemaphoreGive(s_lock);
    return ret;
}
