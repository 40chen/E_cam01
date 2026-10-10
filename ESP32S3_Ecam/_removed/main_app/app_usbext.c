/*
 * app_usbext.c — USB 副屏模式实现
 *
 * 数据通路：
 *
 *   电脑 ──vendor 批量 OUT──> tud_vendor_rx_cb（TinyUSB 任务）
 *                                 │ 拼帧（16 字节头 + JPEG 载荷）
 *                                 ▼
 *                            filled 队列
 *                                 │
 *                            usbext 渲染任务 ──> esp_jpeg_decode ──> 分条画到 ST7789
 *
 * 退出只有一条路：渲染任务在等帧的超时里轮询 BOOT 键。屏幕上没有 UI，所以
 * 那个轮询**必须**一直有机会跑到——本文件里所有阻塞等待都带超时，没有一处
 * portMAX_DELAY，否则任务一旦卡住，按键就永远轮不到，设备只能拔电。
 *
 * 分条画的那套（为什么是 2 块缓冲、为什么不需要完成回调）见 draw_frame()。
 */
#include "app_usbext.h"

#include <string.h>

#include "esp_heap_caps.h"
#include "esp_lcd_panel_ops.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_lvgl_port.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

#include "tusb.h"
#include "tinyusb.h"
#include "tinyusb_default_config.h"

#include "jpeg_decoder.h"

#include "app_audio.h"
#include "app_camera.h"
#include "app_module.h"
#include "app_usbmsc.h"
#include "lckfb_bsp.h"
#include "ui.h"
#include "ui_theme.h"

static const char *TAG = "usbext";

/*---------------------------------------------------------------------------
 * 常量
 *-------------------------------------------------------------------------*/

/* 上位机推过来的必须是整屏。x/y 恒为 0，宽高必须正好是面板尺寸 */
#define USBEXT_W  BSP_LCD_H_RES          /* 320 */
#define USBEXT_H  BSP_LCD_V_RES          /* 240 */

/*
 * 一条多少行。240 / 20 = 12 条整除——**这个整除关系是硬要求**（下面有 static
 * assert 盯着），而且必须是**偶数**（2 块缓冲交替，见 draw_frame）。
 *
 * **为什么是 20 而不是 40**：一行 320 像素 = 640 字节，20 行 = 12,800 字节，
 * 正好是这块板子上**已经验证过能分配出来**的内部 DMA 块大小——`lcd_clear_black()`
 * 的 CHUNK_LINES 和 LVGL 的 BSP_LCD_DRAW_BUF_HEIGHT 都是 20。一开始按 40 行
 * （25,600 字节 × 2 块）要，上板直接 ESP_ERR_NO_MEM：进来这个模式的时候 LVGL
 * 那两块还没释放，内部 DMA 内存不够。
 *
 * 12,800 字节一次 SPI 事务没有超 max_transfer_sz（25,600），不用驱动再拆。
 * 代价只是 12 次 draw 而不是 6 次，每次之间有 CASET/RASET 的轮询开销——相比
 * USB 那条链路的瓶颈可以忽略。
 */
#define USBEXT_STRIP_LINES 20
#define USBEXT_STRIPS      (USBEXT_H / USBEXT_STRIP_LINES)

_Static_assert(USBEXT_STRIPS % 2 == 0, "strip count must be even for the 2-buffer alternation");

/*
 * 单帧 JPEG 上限。质量 6 的 320x240 大约 12~20KB，32KB 留了足够余量。
 * 超过这个值**整帧丢掉**（见 rx_consume 里的 SKIP）——不要试着塞，塞进去的
 * 后果是缓冲溢出 + 画面撕裂，比丢一帧难查得多。
 */
#define USBEXT_JPEG_MAX (32 * 1024)

/* 拼帧缓冲个数。够电脑在我们解码期间继续推几帧；再多纯属白占 PSRAM */
#define USBEXT_FRAMES 3

/* 端点。IN 端点在描述符里必须有，但我们从不往里写 */
#define USBEXT_EP_OUT 0x01
#define USBEXT_EP_IN  0x81

/* 退出判定：照抄 page_camera.c 的 Boot 键去抖参数 */
#define USBEXT_KEY_SAMPLES      3       /* 3 × 50ms = 150ms 确认按下 */

/* 等帧的超时。它同时决定 BOOT 键的轮询节奏，所以别调大 */
#define USBEXT_FRAME_WAIT_MS    50

_Static_assert(BSP_LCD_V_RES % USBEXT_STRIP_LINES == 0,
               "strip height must divide the panel height, or the last rows never refresh");

/*---------------------------------------------------------------------------
 * 上位机推过来的帧头
 *
 * 布局**照抄参考实现 for_example/usb_extend_screen_s3/main/app_vendor.c**——
 * 那边对的是同一份 Windows IDD 驱动，一个字节都不能动。
 *
 * 16 字节，packed，小端：
 *   0  crc16           设备端不校验（驱动自己带的，给它那头用）
 *   2  type            只认 3 = JPG（见下）
 *   3  cmd             设备端不解释
 *   4  x, y            恒为 0
 *   8  width, height   必须等于面板尺寸
 *   12 frame_id:10     没用
 *      payload_total:22  后面跟的 JPEG 字节数（位域高 22 位）
 *-------------------------------------------------------------------------*/
#define USBEXT_TYPE_JPG 3

typedef struct {
    uint16_t crc16;
    uint8_t  type;
    uint8_t  cmd;
    uint16_t x;
    uint16_t y;
    uint16_t width;
    uint16_t height;
    uint32_t frame_id:10;
    uint32_t payload_total:22;
} __attribute__((packed)) usbext_frame_header_t;

/*
 * 这个断言是**故意的**：整个协议都建立在"帧头 16 字节"上，而位域宽度、
 * packed 对齐任何一样变了它就会变成别的值。参考项目的注释里写着 18——那是
 * 数错了（2+1+1+2+2+2+2 = 12，加 4 字节位域字 = 16）。真有偏差的话，
 * 让它在这里编译失败，比在板子上表现为"画面永远对不上"好查一万倍。
 */
_Static_assert(sizeof(usbext_frame_header_t) == 16, "frame header must be 16 bytes");

/*---------------------------------------------------------------------------
 * USB 描述符
 *
 * **VID/PID 和接口号不是随便填的**：预编译的 Windows IDD 驱动按
 * USB\VID_303A&PID_2986&MI_00 匹配，改任何一个都得重新编译那个驱动。
 * 所以 vendor 接口必须是接口 0，VID/PID 必须是这两个。
 *
 * U 盘模式用的是 PID 0x4002，两者不冲突（而且两个模式本来就互斥，见 enter）。
 *-------------------------------------------------------------------------*/
static const tusb_desc_device_t s_device_desc = {
    .bLength = sizeof(s_device_desc),
    .bDescriptorType = TUSB_DESC_DEVICE,
    .bcdUSB = 0x0200,
    /* 只有一个 vendor 接口，不需要 IAD，设备级类别留 0 由接口自己声明 */
    .bDeviceClass = 0x00,
    .bDeviceSubClass = 0x00,
    .bDeviceProtocol = 0x00,
    .bMaxPacketSize0 = CFG_TUD_ENDPOINT0_SIZE,
    .idVendor = 0x303A,
    .idProduct = 0x2986,
    .bcdDevice = 0x0100,
    .iManufacturer = 0x01,
    .iProduct = 0x02,
    .iSerialNumber = 0x03,
    .bNumConfigurations = 0x01,
};

static const uint8_t s_config_desc[] = {
    /* 配置号, 接口数, 字符串索引, 总长, 属性, 供电电流 mA */
    TUD_CONFIG_DESCRIPTOR(1, 1, 0, TUD_CONFIG_DESC_LEN + TUD_VENDOR_DESC_LEN, 0, 100),
    /* 接口号（必须是 0）, 字符串索引, OUT 端点, IN 端点, 端点大小（S3 全速 64） */
    TUD_VENDOR_DESCRIPTOR(0, 4, USBEXT_EP_OUT, 0x80 | USBEXT_EP_IN, 64),
};

/*
 * **索引 4 那个字符串是功能性的，不是给人看的**：IDD 驱动从它里面读分辨率
 * （`R<宽>x<高>`）和 JPEG 质量（`Ejpg<q>`，数字越大质量越高）。
 *
 * 前缀 `esp32s3udisp0_` 是照参考项目的 `esp32p4udisp0_` 改的——**驱动到底怎么
 * 解析这个串没人能保证**（参考只说了"通过接口字符描述符控制"，没说语法）。
 * 如果板子能枚举、驱动也装上了、就是没画面，第一个要试的就是换这个串：
 * 先换成参考原样的 `esp32p4udisp0_R320x240_Ejpg6`，再换成完全一样的
 * `esp32p4udisp0_R240x240_Ejpg6`，两步就能分清是前缀的问题还是分辨率的问题。
 *
 * 质量 6 可以调低换帧率（帧更小、更糊）。见 README 里实测的帧率。
 */
static const char *s_string_desc[] = {
    (const char[]){ 0x09, 0x04 },       /* 0: 语言 = 英语(0x0409) */
    "ERDCX",                            /* 1: 厂商 */
    "ESP32S3_Ecam USB Display",         /* 2: 产品名 */
    "0001",                             /* 3: 序列号 */
    "esp32s3udisp0_R320x240_Ejpg6",     /* 4: Vendor 接口 —— 驱动从这里读参数 */
};

/*---------------------------------------------------------------------------
 * 状态
 *-------------------------------------------------------------------------*/

/* 队列里传的东西。**长度要跟着走**：esp_jpeg_decode 是按 indata_size 去读的，
 * 给多了它会读进缓冲尾巴上没写过的旧数据 */
typedef struct {
    int    idx;         /* s_jpeg[] 的下标 */
    size_t len;         /* 实际攒到多少字节 */
} usbext_frame_t;

static TaskHandle_t  s_task;
static volatile bool s_exit;
static bool          s_usb_installed;

/* 拼帧缓冲 */
static uint8_t      *s_jpeg[USBEXT_FRAMES];
static QueueHandle_t s_empty_q;     /* 可写的拼帧缓冲下标 */
static QueueHandle_t s_filled_q;    /* 攒好待解码的帧 */

/* 渲染用的缓冲 */
static uint8_t  *s_decoded;                     /* PSRAM，RGB565 整帧 */
static uint16_t *s_strip[2];                    /* 内部 DMA 内存，分条画用 */

/*
 * 拼帧状态机。**只有 tud_vendor_rx_cb 碰这些**（串行在 TinyUSB 任务里），
 * 所以不需要锁。
 */
typedef enum {
    RX_HEADER = 0,      /* 攒帧头 */
    RX_PAYLOAD,         /* 攒 JPEG 载荷 */
    RX_SKIP,            /* 丢掉这一帧剩下的字节 */
    RX_RESYNC,          /* 帧头不合法：丢掉本次 read 剩下的，下次重新找头 */
} rx_state_t;

static rx_state_t s_rx_state = RX_HEADER;
static uint8_t    s_hdr[sizeof(usbext_frame_header_t)];
static size_t     s_hdr_len;
static int        s_cur = -1;           /* 正在写的拼帧缓冲下标，-1 = 没有 */
static size_t     s_payload_got;
static size_t     s_payload_total;
static size_t     s_skip_left;

/* BOOT 键去抖状态（只在渲染任务里用） */
static int  s_key_count;
static bool s_key_armed = true;

/*
 * 诊断用的一次性标志。**这几条是为了让"什么都没有发生"也能被区分出来**：
 * 这个模式跑起来之后屏幕是冻住的（ui_suspend 停刷新不清屏，面板上还留着
 * LVGL 最后画的那一帧），所以"没画面"至少有三种完全不同的原因——
 *   a) 电脑根本没在推（驱动装好了、显示器也认了，但没设成扩展/复制）
 *   b) 推了，但帧头校验没过（分辨率/格式对不上）
 *   c) 推了也过了，但解码失败
 * 没有日志的话这三种在板子上长得一模一样。下面几条把 a 和 b 分开。
 */
static bool s_logged_first_chunk;
static bool s_logged_panel_missing;

/*---------------------------------------------------------------------------
 * 缓冲
 *-------------------------------------------------------------------------*/
static void bufs_free(void)
{
    for (int i = 0; i < USBEXT_FRAMES; i++) {
        free(s_jpeg[i]);
        s_jpeg[i] = NULL;
    }
    free(s_decoded);
    s_decoded = NULL;
    for (int i = 0; i < 2; i++) {
        free(s_strip[i]);
        s_strip[i] = NULL;
    }
}

static esp_err_t bufs_alloc(void)
{
    for (int i = 0; i < USBEXT_FRAMES; i++) {
        s_jpeg[i] = heap_caps_malloc(USBEXT_JPEG_MAX, MALLOC_CAP_SPIRAM);
        if (s_jpeg[i] == NULL) {
            ESP_LOGE(TAG, "no PSRAM for jpeg buffer %d (%u bytes wanted, %u free)",
                     i, (unsigned)USBEXT_JPEG_MAX,
                     (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
            bufs_free();
            return ESP_ERR_NO_MEM;
        }
    }

    /* 解码目标：整屏 RGB565。放 PSRAM——15 万字节放内部 RAM 会把堆挤爆，
     * 而它只被 CPU 读写，不进 DMA（进 DMA 的是下面那两块分条缓冲） */
    s_decoded = heap_caps_malloc((size_t)USBEXT_W * USBEXT_H * 2, MALLOC_CAP_SPIRAM);
    if (s_decoded == NULL) {
        ESP_LOGE(TAG, "no PSRAM for the decode target (%u bytes wanted, %u free)",
                 (unsigned)((size_t)USBEXT_W * USBEXT_H * 2),
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
        bufs_free();
        return ESP_ERR_NO_MEM;
    }

    /*
     * 分条缓冲**必须是内部 DMA 内存**。给 esp_lcd_panel_draw_bitmap() 一个
     * PSRAM 指针的话，SPI 驱动会在**每次传输**里 heap_caps_malloc 一块同样大
     * 的内部内存做 bounce（IDF 5.1.7 spi_master.c 的 setup_priv_desc），
     * 一帧 153KB 那样干必然失败或把堆搞碎。自己拷进内部缓冲就没这回事。
     * 这也正是 lcd_clear_black() 一直在用的做法。
     */
    for (int i = 0; i < 2; i++) {
        s_strip[i] = heap_caps_malloc((size_t)USBEXT_W * USBEXT_STRIP_LINES * 2,
                                      MALLOC_CAP_DMA);
        if (s_strip[i] == NULL) {
            ESP_LOGE(TAG, "no internal DMA memory for strip buffer %d "
                     "(%u bytes wanted, %u free; LVGL still holds its own draw buffers)",
                     i, (unsigned)((size_t)USBEXT_W * USBEXT_STRIP_LINES * 2),
                     (unsigned)heap_caps_get_free_size(MALLOC_CAP_DMA));
            bufs_free();
            return ESP_ERR_NO_MEM;
        }
    }
    return ESP_OK;
}

/*---------------------------------------------------------------------------
 * 收：拼帧状态机
 *
 * 从 TinyUSB 任务上下文调用。**不能假设任何对齐**——全速批量端点是 64 字节，
 * 帧头 16 字节，所以"一次 read 正好给一个帧头"这种假设是错的：帧头可能被
 * 切成两半，上一帧的尾巴也可能和下一帧的头挤在同一个 read 里。
 * 参考实现就是栽在这个假设上（它只看 read 的第一段），这里写成显式状态机。
 *-------------------------------------------------------------------------*/
static void frame_complete(int idx, size_t len)
{
    usbext_frame_t f = { .idx = idx, .len = len };
    if (xQueueSend(s_filled_q, &f, 0) != pdPASS) {
        /* 理论上不该发生：filled 队列和缓冲一样大。真发生了就把这块还回去，
         * 不然缓冲会一块块漏光 */
        ESP_LOGW(TAG, "filled queue full, dropping a complete frame");
        xQueueSend(s_empty_q, &idx, 0);
    }
}

static void rx_consume(const uint8_t *buf, size_t len)
{
    size_t off = 0;

    while (off < len) {
        switch (s_rx_state) {
        case RX_HEADER: {
            size_t need = sizeof(s_hdr) - s_hdr_len;
            size_t take = (len - off < need) ? (len - off) : need;
            memcpy(s_hdr + s_hdr_len, buf + off, take);
            s_hdr_len += take;
            off += take;

            if (s_hdr_len < sizeof(s_hdr)) {
                break;                      /* 帧头还没齐，等下次 read */
            }
            s_hdr_len = 0;

            usbext_frame_header_t h;
            memcpy(&h, s_hdr, sizeof(h));

            /*
             * 校验。任何一条不成立都说明双方没对齐或者参数对不上——
             * **把实际值打出来**，这是排查"R320x240 这个串驱动认没认"
             * 最快的手段：日志里会直接出现驱动真正在推的分辨率。
             */
            if (h.type != USBEXT_TYPE_JPG) {
                ESP_LOGW(TAG, "unexpected frame type %u (want %u = JPG), resync",
                         (unsigned)h.type, (unsigned)USBEXT_TYPE_JPG);
                s_rx_state = RX_RESYNC;
                break;
            }
            if (h.x != 0 || h.y != 0 || h.width != USBEXT_W || h.height != USBEXT_H) {
                ESP_LOGW(TAG, "frame geometry %ux%u@%u,%u does not match panel %dx%d, resync",
                         (unsigned)h.width, (unsigned)h.height,
                         (unsigned)h.x, (unsigned)h.y, USBEXT_W, USBEXT_H);
                s_rx_state = RX_RESYNC;
                break;
            }

            s_payload_total = h.payload_total;
            s_payload_got = 0;

            if (s_payload_total == 0 || s_payload_total > USBEXT_JPEG_MAX) {
                /* 太大就整帧丢。这里必须打日志——症状是"画面卡住"而不是报错，
                 * 没有这行就只能靠猜 */
                ESP_LOGD(TAG, "payload %u bytes does not fit the %d byte buffer, skipping",
                         (unsigned)s_payload_total, USBEXT_JPEG_MAX);
                s_skip_left = s_payload_total;
                s_rx_state = RX_SKIP;
                break;
            }

            /* 拿一块空缓冲。没有就整帧跳过——这就是本模式唯一的流控：
             * 电脑推得比我们解得快时，丢帧而不是积压 */
            int idx;
            if (xQueueReceive(s_empty_q, &idx, 0) != pdPASS) {
                s_skip_left = s_payload_total;
                s_rx_state = RX_SKIP;
                break;
            }
            s_cur = idx;
            s_rx_state = RX_PAYLOAD;
            break;
        }

        case RX_PAYLOAD: {
            size_t need = s_payload_total - s_payload_got;
            size_t take = (len - off < need) ? (len - off) : need;
            memcpy(s_jpeg[s_cur] + s_payload_got, buf + off, take);
            s_payload_got += take;
            off += take;

            if (s_payload_got >= s_payload_total) {
                int idx = s_cur;
                size_t got = s_payload_got;
                s_cur = -1;
                s_rx_state = RX_HEADER;
                frame_complete(idx, got);
            }
            break;
        }

        case RX_SKIP: {
            size_t take = (len - off < s_skip_left) ? (len - off) : s_skip_left;
            s_skip_left -= take;
            off += take;
            if (s_skip_left == 0) {
                s_rx_state = RX_HEADER;
            }
            break;
        }

        case RX_RESYNC:
        default:
            /* 丢掉本次 read 剩下的全部，下次重新找帧头。驱动正常时走不到这里；
             * 真走到的原因只有参数不匹配（上面已经打日志了） */
            off = len;
            s_rx_state = RX_HEADER;
            break;
        }
    }
}

/*---------------------------------------------------------------------------
 * TinyUSB 回调
 *-------------------------------------------------------------------------*/
void tud_vendor_rx_cb(uint8_t idx, const uint8_t *buffer, uint16_t bufsize)
{
    /*
     * 缓冲模式（RX_BUFSIZE > 0）：数据在 FIFO 里，这两个参数是 NULL/0，
     * 要用 tud_vendor_n_read 取。所以这里显式无视它们而不是直接用。
     */
    (void)buffer;
    (void)bufsize;

    /* 正在退出：立刻停手。缓冲马上就要被释放，这里多读一个字节都是踩内存。
     * 不读的话端点会 NAK、主机重试——那点重试是拆机过程中的正常现象 */
    if (s_exit) {
        return;
    }

    uint8_t tmp[512];
    while (tud_vendor_n_available(idx)) {
        uint32_t got = tud_vendor_n_read(idx, tmp, sizeof(tmp));
        if (got == 0) {
            break;
        }
        /* 第一包只打一次。**它的意义是回答"电脑到底有没有在推"**——
         * 有这一行说明 USB 通、电脑在推，问题在后半段；没有这一行，问题在
         * 电脑那头（最常见的是 Windows 没把这块显示器设成"扩展"/"复制"，
         * 驱动装好了但那个显示器是关的，自然一帧都不发） */
        if (!s_logged_first_chunk) {
            s_logged_first_chunk = true;
            ESP_LOGI(TAG, "first USB data: %u bytes, starts %02x %02x %02x %02x %02x %02x %02x %02x",
                     (unsigned)got, tmp[0], tmp[1], tmp[2], tmp[3],
                     tmp[4], tmp[5], tmp[6], tmp[7]);
        }
        rx_consume(tmp, got);
    }
}

/*---------------------------------------------------------------------------
 * 画：分条写进面板
 *
 * **为什么分条**：整帧 153KB 如果直接从 PSRAM 递给 draw_bitmap，SPI 驱动会为
 * 每次传输 malloc 一块同样大的内部内存做 bounce（见 bufs_alloc 里的说明）。
 * 切成 40 行一条（25,600 字节，正好是 BSP 的 max_transfer_sz），自己拷进
 * 内部 DMA 缓冲，驱动就不再申请了。
 *
 * **为什么 2 块缓冲、而且不需要完成回调**：
 * esp_lcd_panel_draw_bitmap 是异步的（esp_lvgl_port 的 flush 就靠
 * on_color_trans_done 才知道画完了）。但我们**不能去注册那个回调**——它对
 * 应的是 esp_lvgl_port 注册在同一个 panel_io 上的 on_color_trans_done，
 * 覆盖掉它会让 LVGL 恢复后永远等不到 flush 完成而卡死。
 *
 * 好在不需要：ST7789 的 draw_bitmap 每次都会先用**轮询**方式发 CASET/RASET，
 * 而 panel_io_spi_tx_param 在发之前会把所有在飞的传输收干净
 * （IDF 5.1.7 esp_lcd_panel_io_spi.c："before issue a polling transaction,
 * need to wait queued transactions finished"）。所以**下一次 draw_bitmap 调用
 * 天然保证了上一次的传输已经结束**。
 *
 * 于是 2 块缓冲交替就够了：写第 i+1 条时用的是另一块，而 draw_bitmap(i+1)
 * 开头那一下会保证第 i 条已经画完。6 条是偶数，条 0 上次用的是 buf0，
 * 下一帧第 0 条又轮到 buf0——中间隔着 5 次 draw，早就完了。
 *
 * 代价是 memcpy 和 SPI 传输不重叠（每条串行）。一帧约 6×(2.6ms 传输 + 0.2ms
 * 拷贝) ≈ 17ms，相对 USB 那条链路（每帧 30ms 以上）完全不是瓶颈，不值得为它
 * 去冒覆盖 LVGL 回调的风险。
 *-------------------------------------------------------------------------*/
static void draw_frame(const uint8_t *rgb565)
{
    esp_lcd_panel_handle_t panel = bsp_display_get_panel();
    if (panel == NULL) {
        /* 理论上到不了这儿（bsp_display_init 在开机时就设好了句柄，之后没人
         * 清它）。但真走到这儿的话，表现是"解码日志一切正常、屏幕就是不动"，
         * 是那种最难查的静默失败——所以宁可吵一次 */
        if (!s_logged_panel_missing) {
            s_logged_panel_missing = true;
            ESP_LOGE(TAG, "no LCD panel handle; frames decode but cannot be drawn");
        }
        return;
    }

    const size_t bytes = (size_t)USBEXT_W * USBEXT_STRIP_LINES * 2;
    for (int i = 0; i < USBEXT_STRIPS; i++) {
        int buf = i & 1;
        int y = i * USBEXT_STRIP_LINES;

        memcpy(s_strip[buf], rgb565 + (size_t)y * USBEXT_W * 2, bytes);

        esp_err_t err = esp_lcd_panel_draw_bitmap(panel, 0, y, USBEXT_W,
                                                  y + USBEXT_STRIP_LINES, s_strip[buf]);
        if (err != ESP_OK) {
            /* 丢掉这一帧剩下的条。不要在这里死等——那会连带把 BOOT 键的
             * 轮询一起饿死 */
            ESP_LOGW(TAG, "strip %d draw failed: %s", i, esp_err_to_name(err));
            return;
        }
    }
}

/*---------------------------------------------------------------------------
 * BOOT 键
 *
 * 恢复相机页那套：轮询 + 连续采样去抖 + "按住只触发一次"。
 * 屏幕上没有 UI，这是唯一的出口，所以这段逻辑挂了就等于设备变砖。
 *-------------------------------------------------------------------------*/
static bool boot_key_pressed_once(void)
{
    if (!bsp_boot_button_pressed()) {
        s_key_count = 0;
        s_key_armed = true;             /* 松开才允许下一次 */
        return false;
    }
    if (!s_key_armed) {
        return false;                   /* 还按着，这一轮已经触发过 */
    }
    if (++s_key_count < USBEXT_KEY_SAMPLES) {
        return false;                   /* 还在去抖窗口内 */
    }
    s_key_count = 0;
    s_key_armed = false;
    return true;
}

/*---------------------------------------------------------------------------
 * 渲染任务
 *
 * **退出善后也在这里做**，所以 app_usbext_exit() 只要置个标志就行，不需要
 * 另外养一个"控制器任务"。tinyusb_driver_uninstall() 必须从非 TinyUSB 任务
 * 调用，从本任务调用是合法的。
 *-------------------------------------------------------------------------*/
static void usbext_task(void *arg)
{
    (void)arg;

    ESP_LOGI(TAG, "USB display running; hold BOOT to exit");

    int64_t fps_start = esp_timer_get_time();
    int fps_count = 0;
    int quiet_ticks = 0;

    while (!s_exit) {
        usbext_frame_t f;
        if (xQueueReceive(s_filled_q, &f, pdMS_TO_TICKS(USBEXT_FRAME_WAIT_MS)) != pdPASS) {
            /* 超时。正常（电脑没在推 / 推得比我们慢）走这里，
             * **BOOT 键就是靠这个间隙才轮得到的**，所以这个超时必须留着 */
            if (boot_key_pressed_once()) {
                ESP_LOGI(TAG, "BOOT pressed, leaving USB display mode");
                s_exit = true;
            }
            /* 一包都没收到过的话每 5 秒说一声。否则"电脑没在推"这件事在串口上
             * 表现为一片安静，和"推了但没解出来"分不开 */
            if (!s_logged_first_chunk && ++quiet_ticks % (5000 / USBEXT_FRAME_WAIT_MS) == 0) {
                ESP_LOGW(TAG, "no USB data yet (%.0fs) - is the PC extending its desktop "
                         "onto this display?",
                         (double)quiet_ticks * USBEXT_FRAME_WAIT_MS / 1000.0);
            }
            continue;
        }
        quiet_ticks = 0;

        esp_jpeg_image_cfg_t cfg = {
            .indata      = s_jpeg[f.idx],
            /* 真实长度，不是缓冲上限——解码器按这个去读，给多了会读进
             * 上一次留下的旧数据（JPEG 末尾虽然有 EOI，但长度不对时
             * 解码器可能先撞上它自己的边界检查而报错） */
            .indata_size = (uint32_t)f.len,
            .outbuf      = s_decoded,
            .outbuf_size = (uint32_t)((size_t)USBEXT_W * USBEXT_H * 2),
            .out_format  = JPEG_IMAGE_FORMAT_RGB565,
            .out_scale   = JPEG_IMAGE_SCALE_0,
            /* 面板要的是大端 RGB565。相册页用同一个值（那儿的理由是 LVGL 的
             * CONFIG_LV_COLOR_16_SWAP），两条路殊途同归。
             * 万一颜色反了，改这一个 bit 就回来 */
            .flags.swap_color_bytes = 1,
        };
        esp_jpeg_image_output_t info = { 0 };

        esp_err_t err = esp_jpeg_decode(&cfg, &info);
        if (err != ESP_OK) {
            /* 电脑那头改了画质/分辨率、或者帧被截断。丢一帧继续，不要停 */
            ESP_LOGW(TAG, "jpeg decode failed (%u bytes): %s",
                     (unsigned)f.len, esp_err_to_name(err));
        } else {
            draw_frame(s_decoded);
            if (++fps_count == 50) {
                int64_t now = esp_timer_get_time();
                ESP_LOGI(TAG, "input fps: %.1f",
                         1000000.0 * 50.0 / (double)(now - fps_start));
                fps_start = now;
                fps_count = 0;
            }
        }

        int idx = f.idx;
        xQueueSend(s_empty_q, &idx, 0);
    }

    /* ---- 善后 ----
     * 顺序是硬的：**先关水龙头，再拆管子**。卸掉 TinyUSB 之后才不会再有
     * tud_vendor_rx_cb 进来碰这些缓冲，这时候 free 才安全 */
    if (s_usb_installed) {
        tinyusb_driver_uninstall();
        s_usb_installed = false;
    }
    bufs_free();

    /*
     * 队列一起删掉。**不能留着下次复用**：退出时 filled 队列里可能还压着没解
     * 码的帧，那些下标永远回不到 empty 队列，每进出一次就少一块可用缓冲——
     * 几次之后 empty 队列空了，就再也收不下任何一帧了（症状是"进去之后一直
     * 黑屏、日志里没有任何报错"）。下次进来重建，代价可以忽略。
     */
    if (s_empty_q != NULL) {
        vQueueDelete(s_empty_q);
        s_empty_q = NULL;
    }
    if (s_filled_q != NULL) {
        vQueueDelete(s_filled_q);
        s_filled_q = NULL;
    }

    /* 屏幕还给 LVGL。ui_resume 会强制整屏重画一次 */
    ui_resume();
    if (lvgl_port_lock(0)) {
        ui_toast_show("USB display off", lv_color_white());
        lvgl_port_unlock();
    }

    ESP_LOGI(TAG, "USB display stopped");

    s_task = NULL;      /* 先置空再删自己：等这个标志的人等的是这一行 */
    vTaskDelete(NULL);
}

/*---------------------------------------------------------------------------
 * 对外接口
 *-------------------------------------------------------------------------*/
bool app_usbext_active(void)
{
    return s_task != NULL;
}

esp_err_t app_usbext_enter(void)
{
    if (s_task != NULL) {
        return ESP_OK;                  /* 已经开着，幂等 */
    }
    /* 两个模式抢同一个 USB 口。用户是在同一页上点这两行的，很容易撞上 */
    if (app_usbmsc_active()) {
        ESP_LOGW(TAG, "U-disk mode is using the USB port");
        return ESP_ERR_INVALID_STATE;
    }

    /* 让出吃资源的那些。相机/音频是懒启动的，退出后各自的页面会重新拉起，
     * 这里不用记着恢复谁（U 盘模式也是这个约定） */
    app_camera_stop();
    app_audio_stop();
    app_module_deselect();

    /*
     * BOOT 键是**唯一的出口**。拿不到它就等于进去出不来，所以这里不是"记个
     * 日志继续"，而是直接拒绝进入——宁可用户点一下没反应，也不能把他关在
     * 一块什么都没有的屏幕后面。幂等，重复调没事。
     */
    if (bsp_boot_button_init() != ESP_OK) {
        ESP_LOGE(TAG, "BOOT button unavailable - refusing to take the screen over, "
                 "there would be no way back out");
        return ESP_FAIL;
    }

    esp_err_t err = bufs_alloc();
    if (err != ESP_OK) {
        return err;
    }

    /* 上次退出时已经删干净了（见 usbext_task 的善后）。真漏下的话说明上一个
     * 渲染任务还没退完，这时候重建会和它抢——所以这里只创建，不覆盖，
     * **而且不能走 fail_queues 去删**（那正是别人还在用的东西） */
    if (s_empty_q != NULL || s_filled_q != NULL) {
        ESP_LOGE(TAG, "stale queues, previous session still tearing down");
        bufs_free();
        return ESP_ERR_INVALID_STATE;
    }
    s_empty_q = xQueueCreate(USBEXT_FRAMES, sizeof(int));
    s_filled_q = xQueueCreate(USBEXT_FRAMES, sizeof(usbext_frame_t));
    if (s_empty_q == NULL || s_filled_q == NULL) {
        ESP_LOGE(TAG, "no memory for the frame queues");
        err = ESP_ERR_NO_MEM;
        goto fail_queues;
    }
    for (int i = 0; i < USBEXT_FRAMES; i++) {
        xQueueSend(s_empty_q, &i, 0);
    }

    /* 清掉上一次留下的拼帧状态（进出多次时它还是旧值） */
    s_hdr_len = 0;
    s_cur = -1;
    s_rx_state = RX_HEADER;
    s_key_count = 0;
    s_key_armed = true;
    s_logged_first_chunk = false;
    s_logged_panel_missing = false;

    {
        tinyusb_config_t tusb_cfg = TINYUSB_DEFAULT_CONFIG();
        tusb_cfg.descriptor.device = &s_device_desc;
        tusb_cfg.descriptor.full_speed_config = s_config_desc;
        tusb_cfg.descriptor.string = s_string_desc;
        tusb_cfg.descriptor.string_count = sizeof(s_string_desc) / sizeof(s_string_desc[0]);
        err = tinyusb_driver_install(&tusb_cfg);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "tinyusb install failed: %s", esp_err_to_name(err));
            goto fail_usb;
        }
    }
    s_usb_installed = true;

    /*
     * **从这一行起屏幕才归我们**。上面每一步都可能失败，而一旦 stop 了 LVGL
     * 又没有任务在画，用户面对的是一块冻住不动的屏幕——所以 suspend 必须排在
     * 所有会失败的步骤之后。
     */
    ui_suspend();

    s_exit = false;
    if (xTaskCreatePinnedToCore(usbext_task, "usbext", 6144, NULL, 5, &s_task, 1) != pdPASS) {
        ESP_LOGE(TAG, "no memory for the render task");
        s_task = NULL;
        ui_resume();                    /* 屏幕得还回去，否则就死在那了 */
        err = ESP_ERR_NO_MEM;
        goto fail_usb;
    }

    ESP_LOGI(TAG, "entering USB display mode");
    return ESP_OK;

fail_usb:
    if (s_usb_installed) {
        tinyusb_driver_uninstall();
        s_usb_installed = false;
    }
fail_queues:
    if (s_empty_q != NULL) {
        vQueueDelete(s_empty_q);
        s_empty_q = NULL;
    }
    if (s_filled_q != NULL) {
        vQueueDelete(s_filled_q);
        s_filled_q = NULL;
    }
    bufs_free();
    return err;
}

void app_usbext_exit(void)
{
    if (s_task == NULL) {
        return;                         /* 没开着 */
    }
    /* 只置标志。善后（卸 USB、释放缓冲、还屏幕）由渲染任务自己做——
     * 那才是唯一能安全拆掉这些东西的地方 */
    s_exit = true;
}
