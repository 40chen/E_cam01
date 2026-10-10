/*
 * page_photos.c — 相册页实现
 *
 * 照片在 TF 卡上是普通 JPEG（QVGA 320x240，frame2jpg 软编码），LVGL 本身
 * 解不了 JPEG（CONFIG_LV_USE_SJPG 没开，而且 LVGL 8.3 的 SJPG 只认它自己的
 * 分片格式），所以这里用 esp_jpeg 解成裸 RGB565，再挂进 lv_img_dsc_t。
 * 这条路 page_camera.c 的实时预览已经在用了。
 *
 * esp_jpeg 的 out_scale 能直接解码到 1/2、1/4、1/8，缩略图因此不用
 * "先解全图再缩小"：1/4 出来就是 80x60，正好放进 97x73 的格子。
 */
#include "page_photos.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "jpeg_decoder.h"

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "app_mail.h"
#include "app_storage.h"
#include "lckfb_bsp.h"      /* 大图解码要用屏幕尺寸 BSP_LCD_H_RES/V_RES */
#include "ui.h"
#include "ui_theme.h"

static const char *TAG = "page_photos";

/*---------------------------------------------------------------------------
 * 尺寸
 *
 * 内容区宽 320 - 2*8(pad) = 304。3 列 97 + 2 个 6 的间隙 = 303，正好塞下。
 *-------------------------------------------------------------------------*/
#define GRID_GAP      6
#define CELL_W        97
#define CELL_H        73            /* 4:3，和图幅一致 */
/* 缩略图的目标尺寸。实际解码档位由 jpeg_load() 按源图大小现挑——卡里会
 * 同时有旧的 QVGA 和相机升级后的 VGA 照片，写死档位总有一边不合适 */
#define THUMB_W       80
#define THUMB_H       60
#define TOP_BAR_H     30

#define MAX_PHOTOS    100
#define MAX_JPEG_BYTES (4 * 1024 * 1024)

#define MAIL_TASK_STACK  8192

/*---------------------------------------------------------------------------
 * 状态
 *-------------------------------------------------------------------------*/
typedef struct {
    char          name[64];
    lv_obj_t     *cell;         /* 网格里的格子（可点的那一层） */
    lv_obj_t     *img;          /* 格子里的缩略图对象 */
    lv_img_dsc_t  dsc;          /* 必须常驻：lv_img_set_src 保存的是这个指针 */
    uint8_t      *buf;          /* 解码出来的 RGB565（PSRAM） */
    bool          selected;     /* 批量选择模式下是否被选中 */
} photo_t;

static lv_obj_t   *s_shell = NULL;
static lv_obj_t   *s_grid = NULL;
static photo_t    *s_photos = NULL;
static int         s_count = 0;

/* 标题栏里我们自己加的东西（外壳建的返回键和标题在 s_shell 里） */
static lv_obj_t   *s_title_lbl = NULL;
static lv_obj_t   *s_del_btn = NULL;
static lv_obj_t   *s_mail_all_btn = NULL;
static lv_obj_t   *s_all_btn = NULL;        /* 全选/全不选，只在选择模式下露面 */

/* 批量选择 */
static bool        s_select_mode = false;
static int         s_selected_count = 0;

static lv_timer_t *s_thumb_timer = NULL;
static int         s_thumb_next = 0;

/* 大图视图（在 lv_layer_top() 上，不归页面外壳管，destroy 必须显式关掉） */
static lv_obj_t    *s_viewer = NULL;
static lv_obj_t    *s_viewer_img = NULL;
static lv_obj_t    *s_mail_btn = NULL;
static lv_obj_t    *s_viewer_name = NULL;   /* 标题栏里的文件名 */
static lv_obj_t    *s_prev_btn = NULL;
static lv_obj_t    *s_next_btn = NULL;
static int          s_viewer_idx = -1;      /* 正在看第几张，-1 = 没开 */
static lv_img_dsc_t s_viewer_dsc;
static uint8_t     *s_viewer_buf = NULL;
static char         s_viewer_path[96];

/*---------------------------------------------------------------------------
 * JPEG 解码
 *-------------------------------------------------------------------------*/
/* 缩放档位对应的除数 */
static uint16_t scale_div(esp_jpeg_image_scale_t s)
{
    switch (s) {
    case JPEG_IMAGE_SCALE_1_2: return 2;
    case JPEG_IMAGE_SCALE_1_4: return 4;
    case JPEG_IMAGE_SCALE_1_8: return 8;
    default:                   return 1;
    }
}

/*
 * 按目标尺寸挑档位：在"结果不超过目标"的前提下尽量选大的，既填得满又不解
 * 出用不上的分辨率。
 *
 * 不能写死档位：卡里会同时有旧的 QVGA(320x240) 和相机升级后的 VGA(640x480)
 * 照片。写死 1/4 的话 VGA 照片的缩略图是 160x120，塞不进 97x73 的格子；
 * 写死 1/8 的话旧 QVGA 照片只有 40x30，小得看不清。
 */
static esp_jpeg_image_scale_t pick_scale(uint16_t w, uint16_t h,
                                         int target_w, int target_h)
{
    if (w <= target_w && h <= target_h) {
        return JPEG_IMAGE_SCALE_0;
    }
    if (w / 2 <= target_w && h / 2 <= target_h) {
        return JPEG_IMAGE_SCALE_1_2;
    }
    if (w / 4 <= target_w && h / 4 <= target_h) {
        return JPEG_IMAGE_SCALE_1_4;
    }
    return JPEG_IMAGE_SCALE_1_8;
}

/*
 * 解一张 JPEG 到裸 RGB565，缩放到不超过 target_w x target_h。
 * 返回 PSRAM 上的缓冲（调用方负责 free），失败返回 NULL。
 */
static uint8_t *jpeg_load(const char *path, int target_w, int target_h,
                          uint16_t *out_w, uint16_t *out_h)
{
    FILE *f = fopen(path, "rb");
    if (f == NULL) {
        ESP_LOGW(TAG, "open failed: %s", path);
        return NULL;
    }
    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        return NULL;
    }
    long sz = ftell(f);
    rewind(f);
    if (sz <= 0 || sz > MAX_JPEG_BYTES) {
        ESP_LOGW(TAG, "bad jpeg size %ld: %s", sz, path);
        fclose(f);
        return NULL;
    }

    uint8_t *raw = heap_caps_malloc((size_t)sz, MALLOC_CAP_SPIRAM);
    if (raw == NULL) {
        fclose(f);
        return NULL;
    }
    size_t got = fread(raw, 1, (size_t)sz, f);
    fclose(f);
    if (got != (size_t)sz) {
        ESP_LOGW(TAG, "short read on %s", path);
        free(raw);
        return NULL;
    }

    esp_jpeg_image_cfg_t cfg = {
        .indata      = raw,
        .indata_size = (uint32_t)sz,
        .out_format  = JPEG_IMAGE_FORMAT_RGB565,
        .out_scale   = JPEG_IMAGE_SCALE_0,
        /* CONFIG_LV_COLOR_16_SWAP=y，LVGL 期望高低字节与 JPEG 输出相反 */
        .flags.swap_color_bytes = 1,
    };

    /* 先拿源图真实尺寸。get_image_info 返回的 width/height 是直接从 SOF0 读的，
     * 不受 out_scale 影响（只有 output_len 按缩放折算），所以这里能拿到原尺寸
     * 再决定缩几倍 */
    esp_jpeg_image_output_t info = { 0 };
    if (esp_jpeg_get_image_info(&cfg, &info) != ESP_OK) {
        ESP_LOGW(TAG, "not a usable baseline jpeg: %s", path);
        free(raw);
        return NULL;
    }

    cfg.out_scale = pick_scale(info.width, info.height, target_w, target_h);
    uint16_t div = scale_div(cfg.out_scale);
    uint16_t ow = info.width / div;
    uint16_t oh = info.height / div;

    size_t out_sz = (size_t)ow * oh * 2;
    uint8_t *out = heap_caps_malloc(out_sz, MALLOC_CAP_SPIRAM);
    if (out == NULL) {
        ESP_LOGE(TAG, "no mem for %ux%u decode", ow, oh);
        free(raw);
        return NULL;
    }
    cfg.outbuf = out;
    cfg.outbuf_size = (uint32_t)out_sz;

    esp_err_t err = esp_jpeg_decode(&cfg, &info);
    free(raw);                  /* 解码完成后原始 JPEG 就没用了 */
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "decode failed (%s): %s", esp_err_to_name(err), path);
        free(out);
        return NULL;
    }
    if (out_w) *out_w = ow;
    if (out_h) *out_h = oh;
    return out;
}

/*---------------------------------------------------------------------------
 * 缩略图：每拍解一张
 *-------------------------------------------------------------------------*/
static void thumb_timer_cb(lv_timer_t *t)
{
    if (s_photos == NULL || s_thumb_next >= s_count) {
        lv_timer_del(t);
        s_thumb_timer = NULL;
        ESP_LOGI(TAG, "thumbnails done (%d)", s_count);
        return;
    }

    int i = s_thumb_next++;
    char path[sizeof(s_viewer_path)];
    snprintf(path, sizeof(path), APP_PHOTO_DIR "/%s", s_photos[i].name);

    uint16_t w = 0, h = 0;
    uint8_t *buf = jpeg_load(path, THUMB_W, THUMB_H, &w, &h);
    if (buf == NULL) {
        return;                 /* 这张坏了就跳过，下一拍继续 */
    }
    s_photos[i].buf = buf;
    if (s_photos[i].img != NULL) {
        s_photos[i].dsc.header.cf = LV_IMG_CF_TRUE_COLOR;
        s_photos[i].dsc.header.always_zero = 0;
        s_photos[i].dsc.header.w = (lv_coord_t)w;
        s_photos[i].dsc.header.h = (lv_coord_t)h;
        s_photos[i].dsc.data_size = (uint32_t)w * h * 2;
        s_photos[i].dsc.data = buf;
        lv_img_set_src(s_photos[i].img, &s_photos[i].dsc);
    }
}

/*---------------------------------------------------------------------------
 * 大图视图
 *-------------------------------------------------------------------------*/
static void viewer_close(void);

static void free_buf_async(void *arg)
{
    free(arg);
}

static void viewer_back_cb(lv_event_t *e)
{
    (void)e;
    viewer_close();
}

static void mail_done_async(void *arg)
{
    app_mail_result_t r = (app_mail_result_t)(intptr_t)arg;
    if (s_mail_btn != NULL) {
        lv_obj_clear_state(s_mail_btn, LV_STATE_DISABLED);
    }
    ui_toast_show(app_mail_result_str(r),
                  r == APP_MAIL_OK ? lv_color_white() : UI_COLOR_DANGER);
}

static void mail_task(void *arg)
{
    char *path = (char *)arg;
    app_mail_result_t r = app_mail_send_photo(path);
    free(path);
    lv_async_call(mail_done_async, (void *)(intptr_t)r);
    vTaskDelete(NULL);
}

static void viewer_mail_cb(lv_event_t *e)
{
    (void)e;

    const char *missing = app_mail_missing_field();
    if (missing != NULL) {
        char buf[48];
        snprintf(buf, sizeof(buf), "Set %s first", missing);
        ui_toast_show(buf, UI_COLOR_DANGER);
        return;
    }

    /* 路径要拷一份给任务：本层的 s_viewer_path 可能被下一次选中覆盖 */
    char *path = strdup(s_viewer_path);
    if (path == NULL) {
        ui_toast_show("Out of memory", UI_COLOR_DANGER);
        return;
    }

    if (s_mail_btn != NULL) {
        lv_obj_add_state(s_mail_btn, LV_STATE_DISABLED);
    }
    ui_toast_show("Sending...", lv_color_white());

    if (xTaskCreate(mail_task, "mail", MAIL_TASK_STACK, path, 5, NULL) != pdPASS) {
        free(path);
        if (s_mail_btn != NULL) {
            lv_obj_clear_state(s_mail_btn, LV_STATE_DISABLED);
        }
        ui_toast_show("Out of memory", UI_COLOR_DANGER);
    }
}

static void viewer_close(void)
{
    if (s_viewer != NULL) {
        /* 内部就是 lv_async_call，先进队列 */
        lv_obj_del_async(s_viewer);
        s_viewer = NULL;
        s_viewer_img = NULL;
        s_mail_btn = NULL;
        s_viewer_name = NULL;
        s_prev_btn = NULL;
        s_next_btn = NULL;
        s_viewer_idx = -1;
    }
    if (s_viewer_buf != NULL) {
        /* 后进队列 → 先删控件再释放缓冲。顺序反了的话，LVGL 可能在
         * 真正删除控件之前还去画一次，读的就是已释放的内存 */
        lv_async_call(free_buf_async, s_viewer_buf);
        s_viewer_buf = NULL;
    }
}

/*---------------------------------------------------------------------------
 * 批量选择与删除
 *
 * 入口是标题栏上常驻的 Delete 按钮：点一下进选择模式，再点缩略图切换选中，
 * 选好之后再点 Delete 弹二次确认。同一个按钮承担"进入"和"确认"两个角色，
 * 靠 s_select_mode 区分。
 *
 * 最初做的是长按缩略图进入，实测手感不好：要按住 400ms，而且网格是滚动区，
 * 长按和拖动容易混在一起。
 *-------------------------------------------------------------------------*/

static void set_selected(int idx, bool on);

static void update_title(void)
{
    if (s_title_lbl == NULL) {
        return;
    }
    if (!s_select_mode) {
        lv_label_set_text(s_title_lbl, "Photos");
        return;
    }
    char buf[24];
    snprintf(buf, sizeof(buf), "%d selected", s_selected_count);
    lv_label_set_text(s_title_lbl, buf);
}

static void update_buttons(void)
{
    if (s_del_btn == NULL || s_mail_all_btn == NULL || s_all_btn == NULL) {
        return;
    }

    /*
     * 三个按钮平级，都在标题栏右侧：
     *   - 网格模式：Email / Delete 点任一个都进入选择模式（只要相册里有照片就能点）
     *   - 选择模式：点哪个就执行哪个（邮件 / 删除 / 全选），一张没选时前两个不给按
     * 不需要额外记"这次进来是想干嘛"——按钮自己就说明了。
     */
    bool usable = s_select_mode ? (s_selected_count > 0) : (s_count > 0);

    if (usable) {
        lv_obj_clear_state(s_del_btn, LV_STATE_DISABLED);
        lv_obj_clear_state(s_mail_all_btn, LV_STATE_DISABLED);
    } else {
        lv_obj_add_state(s_del_btn, LV_STATE_DISABLED);
        lv_obj_add_state(s_mail_all_btn, LV_STATE_DISABLED);
    }

    /*
     * "全选"只在选择模式下露面：网格模式下它没有意义，而且三个按钮并排会把
     * 标题挤没（上一版就是这么被删掉的）。选择模式下标题变成 "N selected"，
     * 短得多，腾得出位置。
     * 文字跟着当前状态变：还有没选中的显示 All，全选满了显示 None——一个
     * 按钮兼两个方向，用户不用猜点下去是选还是取消。
     */
    if (!s_select_mode) {
        lv_obj_add_flag(s_all_btn, LV_OBJ_FLAG_HIDDEN);
        return;
    }
    lv_obj_clear_flag(s_all_btn, LV_OBJ_FLAG_HIDDEN);
    lv_label_set_text(lv_obj_get_child(s_all_btn, 0),
                      (s_selected_count < s_count) ? "All" : "None");
    if (s_count > 0) {
        lv_obj_clear_state(s_all_btn, LV_STATE_DISABLED);
    } else {
        lv_obj_add_state(s_all_btn, LV_STATE_DISABLED);
    }
}

/* 全选 / 全不选。已经全选中了再点一次就是全部取消，所以不需要两个按钮 */
static void all_clicked_cb(lv_event_t *e)
{
    (void)e;
    const bool select_all = (s_selected_count < s_count);
    for (int i = 0; i < s_count; i++) {
        set_selected(i, select_all);
    }
}

static void set_selected(int idx, bool on)
{
    if (s_photos == NULL || idx < 0 || idx >= s_count) {
        return;
    }
    if (s_photos[idx].selected == on) {
        return;
    }
    s_photos[idx].selected = on;
    s_selected_count += on ? 1 : -1;

    /* 选中态用格子底色表示。图片只占格子中间 80x60，四周那圈边框会整圈
     * 变成强调色，比加个勾标清楚，也少一堆控件 */
    if (s_photos[idx].cell != NULL) {
        lv_obj_set_style_bg_color(s_photos[idx].cell,
                                  on ? UI_COLOR_ACCENT : UI_COLOR_CARD, 0);
    }
    update_title();
    update_buttons();
}

static void enter_select_mode(void)
{
    if (s_select_mode) {
        return;
    }
    s_select_mode = true;
    s_selected_count = 0;
    update_title();
    update_buttons();
}

static void exit_select_mode(void)
{
    if (!s_select_mode) {
        return;
    }
    for (int i = 0; i < s_count; i++) {
        s_photos[i].selected = false;
        if (s_photos[i].cell != NULL) {
            lv_obj_set_style_bg_color(s_photos[i].cell, UI_COLOR_CARD, 0);
        }
    }
    s_select_mode = false;
    s_selected_count = 0;
    update_title();
    update_buttons();
}

/*---------------------------------------------------------------------------
 * 大图视图
 *
 * viewer_open() 建壳子，viewer_show() 往里面装第 idx 张。分开是为了左右
 * 切换：换一张只需要重装图，不该把整个覆盖层拆了重来。
 *-------------------------------------------------------------------------*/
static void viewer_show(int idx)
{
    if (s_viewer == NULL || s_photos == NULL || idx < 0 || idx >= s_count) {
        return;
    }

    snprintf(s_viewer_path, sizeof(s_viewer_path), APP_PHOTO_DIR "/%s",
             s_photos[idx].name);

    /* 解到不超过屏幕尺寸（QVGA 照片就是原尺寸，VGA 照片解 1/2）。
     * 同步做，VGA 那张约两三百毫秒 */
    uint16_t w = 0, h = 0;
    uint8_t *buf = jpeg_load(s_viewer_path, BSP_LCD_H_RES, BSP_LCD_V_RES, &w, &h);
    if (buf == NULL) {
        /* 解不出来就保持原来那张，不要把视图搞成空的 */
        ui_toast_show("Cannot open photo", UI_COLOR_DANGER);
        return;
    }

    uint8_t *old = s_viewer_buf;
    s_viewer_buf = buf;

    memset(&s_viewer_dsc, 0, sizeof(s_viewer_dsc));
    s_viewer_dsc.header.cf = LV_IMG_CF_TRUE_COLOR;
    s_viewer_dsc.header.w = (lv_coord_t)w;
    s_viewer_dsc.header.h = (lv_coord_t)h;
    s_viewer_dsc.data_size = (uint32_t)w * h * 2;
    s_viewer_dsc.data = buf;
    lv_img_set_src(s_viewer_img, &s_viewer_dsc);

    if (s_viewer_name != NULL) {
        lv_label_set_text(s_viewer_name, s_photos[idx].name);
    }
    s_viewer_idx = idx;

    /* 到头就把对应箭头置灰，别让人点了个没反应的按钮 */
    if (s_prev_btn != NULL) {
        if (idx > 0) {
            lv_obj_clear_state(s_prev_btn, LV_STATE_DISABLED);
        } else {
            lv_obj_add_state(s_prev_btn, LV_STATE_DISABLED);
        }
    }
    if (s_next_btn != NULL) {
        if (idx < s_count - 1) {
            lv_obj_clear_state(s_next_btn, LV_STATE_DISABLED);
        } else {
            lv_obj_add_state(s_next_btn, LV_STATE_DISABLED);
        }
    }

    /* 旧缓冲推迟释放：现在还在按钮的事件回调里，这一轮 LVGL 可能还会画一次。
     * 图片已经指向新缓冲了，但让 free 排在当前处理之后最稳 */
    if (old != NULL) {
        lv_async_call(free_buf_async, old);
    }
}

static void viewer_prev_cb(lv_event_t *e)
{
    (void)e;
    viewer_show(s_viewer_idx - 1);
}

static void viewer_next_cb(lv_event_t *e)
{
    (void)e;
    viewer_show(s_viewer_idx + 1);
}

/* 左右两侧的切换箭头，压在图上面 */
static lv_obj_t *make_arrow_btn(lv_obj_t *parent, const char *symbol,
                                bool left_side, lv_event_cb_t cb)
{
    lv_obj_t *btn = lv_btn_create(parent);
    lv_obj_set_size(btn, 30, 64);
    lv_obj_align(btn, left_side ? LV_ALIGN_LEFT_MID : LV_ALIGN_RIGHT_MID,
                 left_side ? 2 : -2, 0);
    lv_obj_set_style_bg_color(btn, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(btn, LV_OPA_40, 0);
    lv_obj_set_style_radius(btn, UI_RADIUS_BTN, 0);
    lv_obj_set_style_shadow_width(btn, 0, 0);
    lv_obj_set_ext_click_area(btn, 8);
    lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *lbl = lv_label_create(btn);
    lv_label_set_text(lbl, symbol);
    lv_obj_set_style_text_font(lbl, UI_FONT_TITLE, 0);
    lv_obj_set_style_text_color(lbl, lv_color_white(), 0);
    lv_obj_center(lbl);
    return btn;
}

static void viewer_open(int idx)
{
    if (s_viewer != NULL) {
        return;                 /* 已经开着一个了 */
    }
    if (s_photos == NULL || idx < 0 || idx >= s_count) {
        return;
    }

    s_viewer_buf = NULL;
    s_viewer_idx = -1;
    s_viewer_path[0] = '\0';

    /* 盖在整页之上。用 lv_layer_top() 而不是挂到 s_shell 下：页面外壳
     * 根对象是 flex 纵向布局，全屏子对象会被 flex 重新摆位，压不住标题栏 */
    s_viewer = lv_obj_create(lv_layer_top());
    lv_obj_set_size(s_viewer, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_color(s_viewer, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(s_viewer, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(s_viewer, 0, 0);
    lv_obj_set_style_radius(s_viewer, 0, 0);
    lv_obj_set_style_pad_all(s_viewer, 0, 0);
    lv_obj_clear_flag(s_viewer, LV_OBJ_FLAG_SCROLLABLE);

    s_viewer_img = lv_img_create(s_viewer);
    lv_obj_center(s_viewer_img);

    /* 顶部条：返回 + 文件名 + 发邮件 */
    lv_obj_t *bar = lv_obj_create(s_viewer);
    lv_obj_set_size(bar, LV_PCT(100), TOP_BAR_H);
    lv_obj_align(bar, LV_ALIGN_TOP_MID, 0, 0);
    lv_obj_set_style_bg_color(bar, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(bar, LV_OPA_60, 0);
    lv_obj_set_style_border_width(bar, 0, 0);
    lv_obj_set_style_radius(bar, 0, 0);
    lv_obj_set_style_pad_all(bar, 0, 0);
    lv_obj_clear_flag(bar, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *back = lv_btn_create(bar);
    lv_obj_set_size(back, 34, TOP_BAR_H - 4);
    lv_obj_align(back, LV_ALIGN_LEFT_MID, 4, 0);
    lv_obj_set_style_bg_opa(back, LV_OPA_TRANSP, 0);
    lv_obj_set_style_shadow_width(back, 0, 0);
    lv_obj_set_ext_click_area(back, 12);    /* 见 ui_theme.c 的说明 */
    lv_obj_add_event_cb(back, viewer_back_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *arrow = lv_label_create(back);
    lv_label_set_text(arrow, LV_SYMBOL_LEFT);
    lv_obj_set_style_text_font(arrow, UI_FONT_TITLE, 0);
    lv_obj_set_style_text_color(arrow, lv_color_white(), 0);
    lv_obj_center(arrow);

    s_viewer_name = lv_label_create(bar);
    lv_label_set_text(s_viewer_name, "");
    lv_obj_set_style_text_font(s_viewer_name, UI_FONT_SMALL, 0);
    lv_obj_set_style_text_color(s_viewer_name, lv_color_white(), 0);
    lv_obj_align(s_viewer_name, LV_ALIGN_LEFT_MID, 42, 0);

    /* 只放图标，不带 "Email" 文字：和相册标题栏那个信封按钮保持一致，
     * 也不占标题栏的宽度（文件名可以显示得更长） */
    s_mail_btn = lv_btn_create(bar);
    lv_obj_set_size(s_mail_btn, 40, TOP_BAR_H - 6);
    lv_obj_align(s_mail_btn, LV_ALIGN_RIGHT_MID, -4, 0);
    lv_obj_set_style_bg_color(s_mail_btn, UI_COLOR_ACCENT, 0);
    lv_obj_set_style_radius(s_mail_btn, UI_RADIUS_BTN, 0);
    lv_obj_set_style_shadow_width(s_mail_btn, 0, 0);
    lv_obj_set_ext_click_area(s_mail_btn, 8);
    lv_obj_add_event_cb(s_mail_btn, viewer_mail_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *mail_lbl = lv_label_create(s_mail_btn);
    lv_label_set_text(mail_lbl, LV_SYMBOL_ENVELOPE);
    lv_obj_set_style_text_font(mail_lbl, UI_FONT_BODY, 0);
    lv_obj_set_style_text_color(mail_lbl, lv_color_white(), 0);
    lv_obj_center(mail_lbl);

    /* 左右切换箭头。压在图片两侧中间，不挤顶部那一条 */
    s_prev_btn = make_arrow_btn(s_viewer, LV_SYMBOL_LEFT, true, viewer_prev_cb);
    s_next_btn = make_arrow_btn(s_viewer, LV_SYMBOL_RIGHT, false, viewer_next_cb);

    viewer_show(idx);
}

static void tile_cb(lv_event_t *e)
{
    int idx = (int)(intptr_t)lv_event_get_user_data(e);
    if (s_photos == NULL || idx < 0 || idx >= s_count) {
        return;
    }

    if (s_select_mode) {
        set_selected(idx, !s_photos[idx].selected);
        return;
    }

    viewer_open(idx);
}

/*---------------------------------------------------------------------------
 * 照片列表
 *-------------------------------------------------------------------------*/
typedef struct {
    photo_t *arr;
    int      count;
    int      cap;
} list_ctx_t;

static bool list_cb(const char *name, void *user)
{
    list_ctx_t *ctx = (list_ctx_t *)user;
    if (ctx->count >= ctx->cap) {
        return false;           /* 满了就停 */
    }
    strlcpy(ctx->arr[ctx->count].name, name, sizeof(ctx->arr[0].name));
    ctx->count++;
    return true;
}

/* 名字是 IMG_YYYYMMDD_HHMMSS.jpg，字典序就是时间序；降序 = 新的在前 */
static int name_cmp_desc(const void *a, const void *b)
{
    return strcmp(((const photo_t *)b)->name, ((const photo_t *)a)->name);
}

/*---------------------------------------------------------------------------
 * 页面
 *-------------------------------------------------------------------------*/
static void build_grid(void)
{
    lv_obj_t *content = ui_page_shell_content(s_shell);
    s_grid = content;           /* 内容区本身就是纵向 flex + 可滚动 */

    if (!app_storage_ready()) {
        lv_obj_t *lbl = lv_label_create(s_grid);
        lv_label_set_text(lbl, "No TF card");
        lv_obj_set_style_text_font(lbl, UI_FONT_BODY, 0);
        lv_obj_set_style_text_color(lbl, UI_COLOR_TEXT_SUB, 0);
        return;
    }

    s_photos = calloc(MAX_PHOTOS, sizeof(photo_t));
    if (s_photos == NULL) {
        ESP_LOGE(TAG, "no mem for photo list");
        return;
    }
    list_ctx_t ctx = { .arr = s_photos, .count = 0, .cap = MAX_PHOTOS };
    app_storage_list(APP_PHOTO_DIR, ".jpg", list_cb, &ctx);
    s_count = ctx.count;

    if (s_count == 0) {
        lv_obj_t *lbl = lv_label_create(s_grid);
        lv_label_set_text(lbl, "No photos yet");
        lv_obj_set_style_text_font(lbl, UI_FONT_BODY, 0);
        lv_obj_set_style_text_color(lbl, UI_COLOR_TEXT_SUB, 0);
        return;
    }
    qsort(s_photos, s_count, sizeof(photo_t), name_cmp_desc);
    ESP_LOGI(TAG, "%d photo(s)", s_count);

    /*
     * 直接把格子放进内容区，把内容区自己的 flex 流改成横向 wrap。
     *
     * 不套一层"高度 LV_SIZE_CONTENT 的 wrap 容器"是有原因的：那个组合
     * 在 LVGL 8 里高度计算容易出问题（算成 0 或者算不出滚动范围）。
     * 内容区的高度是确定的（外壳用 flex_grow 分的），又是可滚动的，
     * 定高容器里放定尺寸子项做 wrap 是最标准的用法。
     */
    lv_obj_set_flex_flow(s_grid, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_flex_align(s_grid, LV_FLEX_ALIGN_SPACE_BETWEEN,
                          LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_column(s_grid, GRID_GAP, 0);

    for (int i = 0; i < s_count; i++) {
        lv_obj_t *cell = lv_obj_create(s_grid);
        lv_obj_set_size(cell, CELL_W, CELL_H);
        lv_obj_set_style_bg_color(cell, UI_COLOR_CARD, 0);
        lv_obj_set_style_bg_opa(cell, LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(cell, 0, 0);
        lv_obj_set_style_radius(cell, UI_RADIUS_BTN, 0);
        lv_obj_set_style_pad_all(cell, 0, 0);
        lv_obj_clear_flag(cell, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(cell, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(cell, tile_cb, LV_EVENT_CLICKED, (void *)(intptr_t)i);
        s_photos[i].cell = cell;

        /* 缩略图：深色底，解码前就是一块占位黑 */
        lv_obj_t *img = lv_img_create(cell);
        lv_obj_set_size(img, THUMB_W, THUMB_H);
        lv_obj_center(img);
        lv_obj_set_style_bg_color(img, lv_color_black(), 0);
        lv_obj_set_style_bg_opa(img, LV_OPA_COVER, 0);
        /* 不让它截走点击：父格子才是可点的那个 */
        lv_obj_clear_flag(img, LV_OBJ_FLAG_CLICKABLE);
        s_photos[i].img = img;
    }

    /*
     * 一拍解一张，避免一次性卡住 LVGL 好几秒。
     *
     * 周期必须明显大于单张解码耗时，否则定时器一到点就立刻再跑，中间不留
     * 空隙——而触摸是在 LVGL 任务里按 LV_INDEV_DEF_READ_PERIOD(30ms) 轮询的
     * （板子没接触摸中断脚），采样间隔会跟着退化到接近解码耗时，快速轻点
     * 就整段落在两次采样之间、根本不被记录，表现成"返回键偶尔失灵"。
     * 解一张约 40-50ms，留到 60ms 才有真实的空隙。
     */
    s_thumb_next = 0;
    s_thumb_timer = lv_timer_create(thumb_timer_cb, 60, NULL);
}

/* 重新扫描并重建网格（删除照片之后用） */
static void reload_grid(void)
{
    /*
     * 顺序要紧：先删控件，再释放缓冲。
     *
     * 反过来的话，那些 lv_img 还拿着已经 free 掉的 dsc.data 指针，虽然中间
     * 不会重绘（同一段代码里连着做），但让控件比它引用的内存先消失更稳妥。
     */
    if (s_thumb_timer != NULL) {
        lv_timer_del(s_thumb_timer);
        s_thumb_timer = NULL;
    }
    if (s_grid != NULL) {
        lv_obj_clean(s_grid);
    }
    if (s_photos != NULL) {
        for (int i = 0; i < s_count; i++) {
            free(s_photos[i].buf);
        }
        free(s_photos);
        s_photos = NULL;
    }
    s_count = 0;
    s_thumb_next = 0;

    /* 选择模式跟着一起退出：重建之后旧的选中状态没有意义了 */
    s_select_mode = false;
    s_selected_count = 0;

    build_grid();

    /* 必须在 build_grid() 之后调：delete 按钮的可用性取决于 s_count，
     * 而那要等重新扫描完才知道。放在前面的话 s_count 还是 0，按钮会被
     * 置灰并且**再也不会恢复**——表现就是"删过一次之后删不动了" */
    update_title();
    update_buttons();
}

/*---------------------------------------------------------------------------
 * 全选 / 删除
 *-------------------------------------------------------------------------*/
static void reload_grid(void);

/* 曾经有个"全选"按钮，和 Email / Delete 挤在标题栏里太满，去掉了。
 * 标题栏只剩两个按钮之后，标题也有足够空间用原来的大字号。 */

/*---------------------------------------------------------------------------
 * 批量发邮件：把选中的照片作为多个附件放进同一封邮件
 *-------------------------------------------------------------------------*/
typedef struct {
    char **paths;
    int    count;
} mail_job_t;

static void mail_job_free(mail_job_t *job)
{
    if (job == NULL) {
        return;
    }
    for (int i = 0; i < job->count; i++) {
        free(job->paths[i]);
    }
    free(job->paths);
    free(job);
}

static void batch_mail_done_async(void *arg)
{
    app_mail_result_t r = (app_mail_result_t)(intptr_t)arg;
    if (s_mail_all_btn != NULL) {
        lv_obj_clear_state(s_mail_all_btn, LV_STATE_DISABLED);
    }
    ui_toast_show(app_mail_result_str(r),
                  r == APP_MAIL_OK ? lv_color_white() : UI_COLOR_DANGER);
}

static void batch_mail_task(void *arg)
{
    mail_job_t *job = (mail_job_t *)arg;
    app_mail_result_t r = app_mail_send_photos((const char *const *)job->paths,
                                               job->count);
    mail_job_free(job);
    lv_async_call(batch_mail_done_async, (void *)(intptr_t)r);
    vTaskDelete(NULL);
}

static void batch_mail_cb(lv_event_t *e)
{
    (void)e;

    /* 网格模式下这一下是"进入选择模式"（和 Delete 按钮一样） */
    if (!s_select_mode) {
        enter_select_mode();
        return;
    }

    if (s_selected_count == 0 || s_photos == NULL) {
        return;
    }

    const char *missing = app_mail_missing_field();
    if (missing != NULL) {
        char buf[48];
        snprintf(buf, sizeof(buf), "Set %s first", missing);
        ui_toast_show(buf, UI_COLOR_DANGER);
        return;
    }

    /* 上限在这里先拦一道：下层只会静默截断，用户会以为全发出去了 */
    if (s_selected_count > APP_MAIL_MAX_ATTACHMENTS) {
        char buf[48];
        snprintf(buf, sizeof(buf), "Max %d photos per mail",
                 APP_MAIL_MAX_ATTACHMENTS);
        ui_toast_show(buf, UI_COLOR_DANGER);
        return;
    }

    /*
     * 把选中的路径拷成一份快照交给任务。任务要跑好几秒（TLS 握手 + 逐张
     * base64），这期间用户可能已经退出选择模式、删了照片、甚至离开了页面——
     * 让任务直接读 s_photos 就是踩已释放内存。
     */
    mail_job_t *job = calloc(1, sizeof(*job));
    if (job == NULL) {
        ui_toast_show("Out of memory", UI_COLOR_DANGER);
        return;
    }
    job->paths = calloc(s_selected_count, sizeof(char *));
    if (job->paths == NULL) {
        free(job);
        ui_toast_show("Out of memory", UI_COLOR_DANGER);
        return;
    }

    for (int i = 0; i < s_count && job->count < s_selected_count; i++) {
        if (!s_photos[i].selected) {
            continue;
        }
        char full[sizeof(s_viewer_path)];
        snprintf(full, sizeof(full), APP_PHOTO_DIR "/%s", s_photos[i].name);
        job->paths[job->count] = strdup(full);
        if (job->paths[job->count] == NULL) {
            break;
        }
        job->count++;
    }
    if (job->count == 0) {
        mail_job_free(job);
        return;
    }

    if (s_mail_all_btn != NULL) {
        lv_obj_add_state(s_mail_all_btn, LV_STATE_DISABLED);
    }
    ui_toast_show("Sending...", lv_color_white());

    if (xTaskCreate(batch_mail_task, "mail", MAIL_TASK_STACK, job, 5, NULL) != pdPASS) {
        mail_job_free(job);
        if (s_mail_all_btn != NULL) {
            lv_obj_clear_state(s_mail_all_btn, LV_STATE_DISABLED);
        }
        ui_toast_show("Out of memory", UI_COLOR_DANGER);
    }
}

static void delete_async(void *arg)
{
    (void)arg;
    if (s_photos == NULL) {
        return;
    }

    int wanted = 0, removed = 0;
    for (int i = 0; i < s_count; i++) {
        if (!s_photos[i].selected) {
            continue;
        }
        wanted++;
        char path[sizeof(s_viewer_path)];
        snprintf(path, sizeof(path), APP_PHOTO_DIR "/%s", s_photos[i].name);
        if (remove(path) == 0) {
            removed++;
        } else {
            ESP_LOGW(TAG, "remove failed: %s", path);
        }
    }
    ESP_LOGI(TAG, "deleted %d of %d selected photo(s)", removed, wanted);

    /* 重新扫一遍而不是从数组里剔除：失败的那几张还在卡上（没插卡、只读、
     * FAT 出问题），重扫最稳，顺带把删除后的新状态一次对齐 */
    reload_grid();

    char msg[48];
    if (removed == wanted) {
        snprintf(msg, sizeof(msg), "Deleted %d photo(s)", removed);
    } else {
        snprintf(msg, sizeof(msg), "Deleted %d of %d", removed, wanted);
    }
    ui_toast_show(msg, removed > 0 ? lv_color_white() : UI_COLOR_DANGER);
}

static void del_confirm_cb(lv_event_t *e)
{
    /*
     * 必须用 get_current_target，不能用 get_target。
     *
     * msgbox 的按钮是 btnmatrix，它发的 VALUE_CHANGED 会**冒泡**到 msgbox
     * 才轮到我们这个回调。冒泡时 LVGL 只往上挪 current_target，
     * e->target 始终是最初发出事件的对象——也就是那个 btnmatrix。
     * 用 get_target 的话拿到的是 btnmatrix，却被当成 lv_msgbox_t 去读 btnm
     * 成员（读的是结构体里另一个偏移），得到野指针后解引用，直接
     * LoadProhibited 复位。
     */
    lv_obj_t *mbox = lv_event_get_current_target(e);
    if (lv_msgbox_get_active_btn(mbox) == 1) {
        /* 真正动手放到下一次 LVGL 处理：现在还在这个 msgbox 的事件回调里，
         * 而删除会重建整个网格 */
        lv_async_call(delete_async, NULL);
    }
    lv_msgbox_close_async(mbox);
}

static void del_cb(lv_event_t *e)
{
    (void)e;

    /* 还没进选择模式：这一下是"进入批量删除" */
    if (!s_select_mode) {
        enter_select_mode();
        return;
    }

    /* 已经在选择模式：这一下是"确认删除" */
    if (s_selected_count == 0) {
        return;
    }
    char text[64];
    snprintf(text, sizeof(text), "Delete %d photo(s)?\nThis cannot be undone.",
             s_selected_count);

    /* 删照片不可恢复，必须二次确认 */
    static const char *btns[] = { "Cancel", "Delete", "" };
    lv_obj_t *mbox = lv_msgbox_create(NULL, "Delete Photos", text, btns, false);
    lv_obj_set_style_text_font(lv_msgbox_get_text(mbox), UI_FONT_SMALL, 0);
    lv_obj_add_event_cb(mbox, del_confirm_cb, LV_EVENT_VALUE_CHANGED, NULL);
}

/*---------------------------------------------------------------------------
 * 页面
 *-------------------------------------------------------------------------*/
static void page_back_cb(lv_event_t *e)
{
    (void)e;
    /* 选择模式下返回键是"取消选择"，不是退出页面 */
    if (s_select_mode) {
        exit_select_mode();
        return;
    }
    ui_go_home_async();
}

lv_obj_t *page_photos_create(lv_obj_t *parent)
{
    /* 用自己的返回键：选择模式下要能取消选择而不是直接退页面 */
    s_shell = ui_page_shell_create_ex(parent, "Photos", page_back_cb);

    /*
     * 往外壳的标题栏里塞两个按钮。外壳的标题栏是 s_shell 的第 0 个子对象，
     * 里面第 0 个是返回键、第 1 个是标题文本。标题栏不是 flex 容器，所以
     * 这里可以绝对定位。
     */
    lv_obj_t *bar = lv_obj_get_child(s_shell, 0);
    if (bar != NULL) {
        s_title_lbl = lv_obj_get_child(bar, 1);

        /*
         * 两个常驻按钮：Email(信封) 在 Delete(垃圾桶) 左侧，平级。
         * 网格模式下点任一个都进选择模式，选择模式下点哪个执行哪个。
         * 用图标而不是英文词：通用语义，也省地方——标题从 x=58 到最左边的
         * 按钮(230)还有 172px，够用大字号。
         */
        s_mail_all_btn = lv_btn_create(bar);
        lv_obj_set_size(s_mail_all_btn, 40, UI_TITLE_BAR_H - 8);
        lv_obj_align(s_mail_all_btn, LV_ALIGN_RIGHT_MID, -50, 0);
        lv_obj_set_style_bg_color(s_mail_all_btn, UI_COLOR_ACCENT, 0);
        lv_obj_set_style_radius(s_mail_all_btn, UI_RADIUS_BTN, 0);
        lv_obj_set_style_shadow_width(s_mail_all_btn, 0, 0);
        /* 不加 HIDDEN：两个按钮平级常驻。它的可用性由 update_buttons() 管 */
        lv_obj_add_event_cb(s_mail_all_btn, batch_mail_cb, LV_EVENT_CLICKED, NULL);
        lv_obj_t *ml = lv_label_create(s_mail_all_btn);
        lv_label_set_text(ml, LV_SYMBOL_ENVELOPE);
        lv_obj_set_style_text_font(ml, UI_FONT_BODY, 0);
        lv_obj_set_style_text_color(ml, lv_color_white(), 0);
        lv_obj_center(ml);

        s_del_btn = lv_btn_create(bar);
        lv_obj_set_size(s_del_btn, 40, UI_TITLE_BAR_H - 8);
        lv_obj_align(s_del_btn, LV_ALIGN_RIGHT_MID, -6, 0);
        lv_obj_set_style_bg_color(s_del_btn, UI_COLOR_DANGER, 0);
        lv_obj_set_style_radius(s_del_btn, UI_RADIUS_BTN, 0);
        lv_obj_set_style_shadow_width(s_del_btn, 0, 0);
        lv_obj_add_event_cb(s_del_btn, del_cb, LV_EVENT_CLICKED, NULL);
        lv_obj_t *dl = lv_label_create(s_del_btn);
        lv_label_set_text(dl, LV_SYMBOL_TRASH);
        lv_obj_set_style_text_font(dl, UI_FONT_BODY, 0);
        lv_obj_set_style_text_color(dl, lv_color_white(), 0);
        lv_obj_center(dl);

        /* 全选按钮在最左边，默认藏起来（只有选择模式才显示，见 update_buttons） */
        s_all_btn = lv_btn_create(bar);
        lv_obj_set_size(s_all_btn, 44, UI_TITLE_BAR_H - 8);
        lv_obj_align(s_all_btn, LV_ALIGN_RIGHT_MID, -96, 0);
        lv_obj_set_style_bg_color(s_all_btn, UI_COLOR_ACCENT, 0);
        lv_obj_set_style_radius(s_all_btn, UI_RADIUS_BTN, 0);
        lv_obj_set_style_shadow_width(s_all_btn, 0, 0);
        lv_obj_add_flag(s_all_btn, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_event_cb(s_all_btn, all_clicked_cb, LV_EVENT_CLICKED, NULL);
        lv_obj_t *al = lv_label_create(s_all_btn);
        lv_label_set_text(al, "All");
        lv_obj_set_style_text_font(al, UI_FONT_SMALL, 0);
        lv_obj_set_style_text_color(al, lv_color_white(), 0);
        lv_obj_center(al);
    }

    build_grid();
    /* 网格建好才知道有没有照片，Delete 按钮的可用性据此定 */
    update_buttons();
    return s_shell;
}

void page_photos_destroy(void)
{
    /* 大图层在 lv_layer_top() 上，不归页面外壳管，必须显式关掉 */
    viewer_close();

    if (s_thumb_timer != NULL) {
        lv_timer_del(s_thumb_timer);
        s_thumb_timer = NULL;
    }

    /* 释放所有缩略图缓冲。注意 lv_img_set_src 还拿着 dsc.data 的指针，
     * 但整个页面对象马上会被 ui.c 删掉，不会再画了 */
    if (s_photos != NULL) {
        for (int i = 0; i < s_count; i++) {
            free(s_photos[i].buf);
            s_photos[i].buf = NULL;
        }
        free(s_photos);
        s_photos = NULL;
    }
    s_count = 0;
    s_thumb_next = 0;
    s_grid = NULL;
    s_shell = NULL;

    /* 选择模式的指针全指向刚被销毁的控件，必须一起清掉。
     * 注意这里不调 update_title/update_buttons——那两个会去动已经不存在的
     * 标签和按钮 */
    s_select_mode = false;
    s_selected_count = 0;
    s_title_lbl = NULL;
    s_del_btn = NULL;
    s_all_btn = NULL;
}
