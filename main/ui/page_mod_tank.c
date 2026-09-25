/*
 * page_mod_tank.c — 模块控制页：履带车
 *
 * 左边一个圆盘摇杆，右边链接状态 / C2 的 IP / 停车 / 推凭据 / 关模块。
 *
 * **摇杆回调只写两个 float**（app_tank_set_vector），发多少、什么时候发归
 * app_tank 的任务——手指划过屏幕每秒能出上百个事件，一个事件一条串口就是
 * 拿事件率当波特率用。
 *
 * **离开这一页会把向量归零，但不停模块**：车停住、心跳继续、链接不断。
 * 这是"跟模式不跟页面"那条规矩里唯一带安全含义的一处。
 */
#include "page_mod_tank.h"

#include <math.h>
#include <stdio.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "qrcode.h"

#include "app_module.h"
#include "app_settings.h"
#include "app_tank.h"
#include "ui.h"
#include "ui_theme.h"

static const char *TAG = "page_mod_tank";

/*
 * 排版。内容区是 304x190，每一个数都是拿 tools/measure_font.py 量出来的：
 *
 *   横：左摇杆 152 + 间距 8 + 右栏 144 = 304
 *       右栏内可用 144 - 2x8（行内边距）= 128
 *         "Disable module"(14 号)  111.8  ✓
 *         "192.168.100.200"(14 号) 104.8  ✓   ← 这行是全场最长的
 *   竖：状态 16 + 地址 16 + 4x5 间距 + STOP 36 + 按钮 30x3 = 188 ≤ 190
 *
 * **这一页不能用 ui_list_row_create**：那是给整屏宽（304）的列表页用的，
 * 标签是 16 号字 + 左右各 12 的内边距，"Disable module" 16 号量出来 127.8px，
 * 塞进 128px 的行里就会压到右边的值文本上——上一版就是这么叠字的。
 * 副屏页踩过同一个坑，README 里写着"这一页不用 ui_list_row_create，
 * 自己排一组紧凑行"，这里是第二次。
 */
#define PAD_SIZE     152
#define KNOB_SIZE    48
#define KNOB_TRAVEL  ((PAD_SIZE - KNOB_SIZE) / 2)   /* 圆心到边缘还剩多少 */
#define COL_W        144
#define STOP_H       34
#define LINK_ROW_H   26

static lv_obj_t   *s_pad;
static lv_obj_t   *s_knob;
static lv_obj_t   *s_lbl_link;      /* 链接状态那行文字 */
static lv_obj_t   *s_lbl_ip;        /* 车的地址 */
static lv_obj_t   *s_row_wifi;      /* 推凭据那行（值要能改，所以留着整行） */
static lv_obj_t   *s_val_wifi;
static lv_timer_t *s_timer;

/*---------------------------------------------------------------------------
 * 二维码：把车的网页地址交给手机
 *
 * 为什么需要它：车没有屏幕，它的网页地址只显示在这块屏上，而让用户拿手指
 * 去敲一串 `192.168.x.x` 是最容易出错的环节——错一位就是"打不开"，还得回来
 * 重新看。扫一下省掉整个环节。
 *
 * 点 Address 那一行弹出来，尺寸按二维码的模块数算（见 qr_render），
 * 四周的静默区**由卡片的留白充当**，不画进点阵里。
 *-------------------------------------------------------------------------*/
#define QR_MAX_PX    150        /* 二维码本身的最大边长 */
#define QR_PAD       16         /* 卡片留白 = 静默区（标准要 4 个模块） */
/* 纯黑白，字节序无所谓：0x0000 和 0xFFFF 在两种字节序下都一样 */
#define QR_DARK      0x0000
#define QR_LIGHT     0xFFFF

static lv_obj_t    *s_qr_overlay;
static lv_img_dsc_t s_qr_dsc;
static uint8_t     *s_qr_buf;

typedef struct {
    uint8_t *buf;       /* 画进去的目标缓冲（RGB565） */
    int      side;      /* 边长（像素） */
} qr_out_t;

/*
 * esp_qrcode_generate 的回调：点阵在这儿落到我们自己的缓冲里。
 *
 * 用它带 user_data 的那版回调（v0.2.0 的联合体里两个都给了），
 * 这样不用为了传参去动文件级变量。
 */
static void qr_render(esp_qrcode_handle_t qr, void *user)
{
    qr_out_t *out = (qr_out_t *)user;
    const int n = esp_qrcode_get_size(qr);
    if (n <= 0 || out == NULL) {
        return;
    }
    int scale = QR_MAX_PX / n;
    if (scale < 3) {
        scale = 3;                      /* 再小就扫不出来了 */
    }
    const int side = n * scale;
    /* **尺寸要先写出去**：第一趟调这个回调时 buf 还是 NULL，就是为了问出边长 */
    out->side = side;

    if (out->buf == NULL) {
        return;                         /* 第一趟：只问尺寸，不画 */
    }

    uint16_t *px = (uint16_t *)out->buf;
    for (int my = 0; my < n; my++) {
        for (int mx = 0; mx < n; mx++) {
            const uint16_t v = esp_qrcode_get_module(qr, mx, my) ? QR_DARK : QR_LIGHT;
            /* 一个模块铺 scale x scale 个像素 */
            for (int dy = 0; dy < scale; dy++) {
                uint16_t *row = px + (size_t)(my * scale + dy) * side + mx * scale;
                for (int dx = 0; dx < scale; dx++) {
                    row[dx] = v;
                }
            }
        }
    }
}

static void qr_close(void)
{
    if (s_qr_overlay != NULL) {
        lv_obj_del(s_qr_overlay);
        s_qr_overlay = NULL;
    }
    if (s_qr_buf != NULL) {
        heap_caps_free(s_qr_buf);
        s_qr_buf = NULL;
    }
    s_qr_dsc.data = NULL;
}

static void qr_backdrop_cb(lv_event_t *e)
{
    (void)e;
    qr_close();
}

static void qr_open(void)
{
    app_tank_status_t st = { 0 };
    app_tank_get_status(&st);
    if (st.ip[0] == '\0') {
        ui_toast_show("No address yet", UI_COLOR_DANGER);
        return;
    }
    char url[40];
    snprintf(url, sizeof(url), "http://%s/", st.ip);

    /* 先编码，拿到实际边长才知道要开多大的缓冲 */
    s_qr_buf = NULL;
    qr_out_t out = { 0 };
    esp_qrcode_config_t cfg = {
        .display_func_with_cb = qr_render,
        .max_qrcode_version = 4,            /* 地址很短，用不着更大的版本 */
        .qrcode_ecc_level = ESP_QRCODE_ECC_LOW,
        .user_data = &out,
    };

    /*
     * 两趟：第一趟只为问出边长（回调里会算出 scale 和 side），
     * 有了尺寸再分配缓冲跑第二趟。
     * 缓冲放 PSRAM：150x150 的 RGB565 是 45KB，内部 RAM 现在是紧的
     * （相机和音频都盯着它），而 LVGL 读 PSRAM 里的图片没问题
     * （相机预览就是这么干的）。
     */
    if (esp_qrcode_generate(&cfg, url) != ESP_OK) {
        ui_toast_show("QR encode failed", UI_COLOR_DANGER);
        return;
    }
    const int side = out.side;
    if (side <= 0) {
        ui_toast_show("QR encode failed", UI_COLOR_DANGER);
        return;
    }
    s_qr_buf = heap_caps_malloc((size_t)side * side * 2, MALLOC_CAP_SPIRAM);
    if (s_qr_buf == NULL) {
        s_qr_buf = heap_caps_malloc((size_t)side * side * 2, MALLOC_CAP_8BIT);
    }
    if (s_qr_buf == NULL) {
        ui_toast_show("Out of memory", UI_COLOR_DANGER);
        return;
    }
    out.buf = s_qr_buf;
    if (esp_qrcode_generate(&cfg, url) != ESP_OK) {
        qr_close();
        ui_toast_show("QR encode failed", UI_COLOR_DANGER);
        return;
    }

    s_qr_dsc.header.cf = LV_IMG_CF_TRUE_COLOR;
    s_qr_dsc.header.always_zero = 0;
    s_qr_dsc.header.w = side;
    s_qr_dsc.header.h = side;
    s_qr_dsc.data_size = (uint32_t)side * side * 2;
    s_qr_dsc.data = s_qr_buf;

    /* ---- 覆盖层：整屏压暗 + 中间一张白卡片 ---- */
    s_qr_overlay = lv_obj_create(lv_layer_top());
    lv_obj_set_size(s_qr_overlay, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_color(s_qr_overlay, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(s_qr_overlay, LV_OPA_60, 0);
    lv_obj_set_style_border_width(s_qr_overlay, 0, 0);
    lv_obj_set_style_radius(s_qr_overlay, 0, 0);
    lv_obj_set_style_pad_all(s_qr_overlay, 0, 0);
    lv_obj_clear_flag(s_qr_overlay, LV_OBJ_FLAG_SCROLLABLE);
    /* 点空白处关掉。背景卡片自己吃掉点击（下面的 card 是可点的，
     * 事件不会冒泡到这里） */
    lv_obj_add_flag(s_qr_overlay, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(s_qr_overlay, qr_backdrop_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *card = lv_obj_create(s_qr_overlay);
    lv_obj_set_size(card, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_color(card, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(card, UI_RADIUS_CARD, 0);
    lv_obj_set_style_border_width(card, 0, 0);
    /* 留白就是二维码的静默区：白底 + 没有别的东西，读数器和人眼都靠它 */
    lv_obj_set_style_pad_all(card, QR_PAD, 0);
    lv_obj_set_style_pad_row(card, 10, 0);
    lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(card, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    /*
     * 卡片**故意不设 CLICKABLE**：LVGL 找的是点击位置下最上层的可点对象，
     * 卡片不可点就会被跳过、落到浮层本身，于是"点哪儿都能关"这一条成立
     * （提示里也是这么写的）。设了的话点卡片中间就没反应了。
     */
    lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_center(card);

    lv_obj_t *img = lv_img_create(card);
    lv_img_set_src(img, &s_qr_dsc);

    lv_obj_t *cap = lv_label_create(card);
    lv_label_set_text(cap, url);
    lv_obj_set_style_text_font(cap, UI_FONT_BODY, 0);
    lv_obj_set_style_text_color(cap, UI_COLOR_TEXT, 0);

    lv_obj_t *hint = lv_label_create(card);
    lv_label_set_text(hint, "Scan to drive from your phone\nTap anywhere to close");
    lv_obj_set_style_text_align(hint, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_font(hint, UI_FONT_SMALL, 0);
    lv_obj_set_style_text_color(hint, UI_COLOR_TEXT_SUB, 0);

    ESP_LOGI(TAG, "二维码: %s（%d 模块 -> %d px）", url, side, side);
}

/*---------------------------------------------------------------------------
 * 摇杆
 *-------------------------------------------------------------------------*/
static void joy_apply(float x, float y)
{
    /* 视觉上把按钮挪过去（translate 不触发重排），再交给服务。
     * 注意 LVGL 的 y 向下，而"前推"是屏幕上方 = +y，所以这里要取反 */
    lv_obj_set_style_translate_x(s_knob, (lv_coord_t)(x * KNOB_TRAVEL), 0);
    lv_obj_set_style_translate_y(s_knob, (lv_coord_t)(-y * KNOB_TRAVEL), 0);
    app_tank_set_vector(x, y);
}

static void joy_cb(lv_event_t *e)
{
    const lv_event_code_t code = lv_event_get_code(e);

    /* 松手 / 手指滑出圆盘 → 立刻归零。**这是安全动作**：
     * 用户手离开屏幕就是"停车"，不是"保持上一档" */
    if (code == LV_EVENT_RELEASED || code == LV_EVENT_PRESS_LOST) {
        joy_apply(0.0f, 0.0f);
        return;
    }

    lv_indev_t *indev = lv_indev_get_act();
    if (indev == NULL) {
        return;
    }
    lv_point_t p;
    lv_indev_get_point(indev, &p);

    lv_area_t a;
    lv_obj_get_content_coords(s_pad, &a);
    const lv_coord_t cx = (a.x1 + a.x2) / 2;
    const lv_coord_t cy = (a.y1 + a.y2) / 2;
    const lv_coord_t r  = (a.x2 - a.x1) / 2;
    if (r <= 0) {
        return;
    }

    float dx = (float)(p.x - cx) / (float)r;
    float dy = (float)(cy - p.y) / (float)r;
    const float len = sqrtf(dx * dx + dy * dy);
    if (len > 1.0f) {
        /* 夹到单位圆上：圆盘不是方盘，斜着推到底不该比正着推更远 */
        dx /= len;
        dy /= len;
    } else if (len < 0.08f) {
        /* 死区：手指搭在上面不动，车不该慢慢往前爬 */
        dx = 0.0f;
        dy = 0.0f;
    }
    joy_apply(dx, dy);
}

/*---------------------------------------------------------------------------
 * 状态刷新
 *-------------------------------------------------------------------------*/
static void tank_refresh(void)
{
    app_tank_status_t st = { 0 };
    app_tank_get_status(&st);

    lv_label_set_text(s_lbl_link, st.up ? LV_SYMBOL_OK " Linked" : "No response");
    lv_obj_set_style_text_color(s_lbl_link, st.up ? UI_COLOR_ACCENT : UI_COLOR_DANGER, 0);

    /* C2 没有屏幕，它那个网页的地址只能显示在这儿——这是用户唯一的入口 */
    lv_label_set_text(s_lbl_ip, st.ip[0] != '\0' ? st.ip : "--");

    if (!app_settings_has_wifi()) {
        /* 设置页里没填过 WiFi——这条不是"没发过"，是"根本没得发"。
         * 文案要短（"Set WiFi first" 量出来 95px，加上标签就顶格了），
         * "该去哪儿填"交给点它之后的提示条说 */
        lv_label_set_text(s_val_wifi, "No WiFi");
        lv_obj_set_style_text_color(s_val_wifi, UI_COLOR_TEXT_SUB, 0);
    } else if (st.wifi_sent) {
        lv_label_set_text(s_val_wifi, "Sent");
        lv_obj_set_style_text_color(s_val_wifi, UI_COLOR_ACCENT, 0);
    } else {
        lv_label_set_text(s_val_wifi, "Not sent");
        lv_obj_set_style_text_color(s_val_wifi, UI_COLOR_ACCENT, 0);
    }
}

static void timer_cb(lv_timer_t *t)
{
    (void)t;
    tank_refresh();
}

/*---------------------------------------------------------------------------
 * 行回调
 *-------------------------------------------------------------------------*/
static void stop_cb(lv_event_t *e)
{
    (void)e;
    app_tank_set_vector(0.0f, 0.0f);
    joy_apply(0.0f, 0.0f);          /* 顺带把圆钮弹回中间 */
}

static void wifi_cb(lv_event_t *e)
{
    (void)e;
    const esp_err_t err = app_tank_send_wifi();
    if (err != ESP_OK) {
        /* 最常见的原因是设置页里还没填过 WiFi（另一条是模块没在跑）。
         * 直接把话说到"去哪儿填"，比抛一个 ESP_ERR_INVALID_STATE 强 */
        ui_toast_show(app_settings_has_wifi() ? "Module not running"
                                              : "Set Wi-Fi in Settings first",
                      UI_COLOR_DANGER);
        return;
    }
    ui_toast_show("Sending Wi-Fi credentials", lv_color_white());
    tank_refresh();
}

static void back_cb(lv_event_t *e)
{
    (void)e;
    ui_open_modules();
}

static void qr_row_cb(lv_event_t *e)
{
    (void)e;
    qr_open();
}

static void disable_cb(lv_event_t *e)
{
    (void)e;
    app_module_deselect();
    ui_open_modules();
}

/*---------------------------------------------------------------------------
 * 页面
 *-------------------------------------------------------------------------*/
/* 卡片里的一行：左边标签、右边值（可选），透明底（卡片本身已经是白的） */
static lv_obj_t *link_row(lv_obj_t *parent, const char *label, const char *value,
                          lv_obj_t **out_value, lv_event_cb_t cb)
{
    lv_obj_t *row = lv_obj_create(parent);
    lv_obj_set_size(row, LV_PCT(100), LINK_ROW_H);
    lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(row, 0, 0);
    lv_obj_set_style_pad_all(row, 0, 0);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *l = lv_label_create(row);
    lv_label_set_text(l, label);
    lv_obj_set_style_text_font(l, UI_FONT_SMALL, 0);
    lv_obj_set_style_text_color(l, UI_COLOR_TEXT, 0);
    lv_obj_align(l, LV_ALIGN_LEFT_MID, 0, 0);

    lv_obj_t *v = lv_label_create(row);
    lv_label_set_text(v, value);
    lv_obj_set_style_text_font(v, UI_FONT_SMALL, 0);
    lv_obj_set_style_text_color(v, UI_COLOR_TEXT_SUB, 0);
    lv_obj_align(v, LV_ALIGN_RIGHT_MID, 0, 0);
    if (out_value != NULL) {
        *out_value = v;
    }
    if (cb != NULL) {
        lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(row, cb, LV_EVENT_CLICKED, NULL);
        /* 能点的行给点提示：值染成强调色（iOS 列表里那个"蓝色可点值"） */
        lv_obj_set_style_text_color(v, UI_COLOR_ACCENT, 0);
    }
    return row;
}

/* 整行按钮，字体 14 */
static lv_obj_t *page_button(lv_obj_t *parent, const char *text, lv_color_t bg,
                            lv_color_t fg, const lv_font_t *font, int h,
                            lv_event_cb_t cb)
{
    lv_obj_t *btn = lv_btn_create(parent);
    lv_obj_set_size(btn, LV_PCT(100), h);
    lv_obj_set_style_radius(btn, 8, 0);
    lv_obj_set_style_bg_color(btn, bg, 0);
    lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, 0);
    lv_obj_set_style_shadow_width(btn, 0, 0);
    lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *l = lv_label_create(btn);
    lv_label_set_text(l, text);
    lv_obj_set_style_text_font(l, font, 0);
    lv_obj_set_style_text_color(l, fg, 0);
    lv_obj_center(l);
    return btn;
}

lv_obj_t *page_mod_tank_create(lv_obj_t *parent)
{
    lv_obj_t *shell = ui_page_shell_create_ex(parent, "Tracked Vehicle", back_cb);
    lv_obj_t *content = ui_page_shell_content(shell);

    lv_obj_t *row = lv_obj_create(content);
    lv_obj_set_size(row, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(row, 0, 0);
    lv_obj_set_style_pad_all(row, 0, 0);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(row, 8, 0);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);

    /* ---- 左：摇杆 ---- */
    lv_obj_t *pad_wrap = lv_obj_create(row);
    lv_obj_set_size(pad_wrap, PAD_SIZE, PAD_SIZE);
    lv_obj_set_style_bg_opa(pad_wrap, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(pad_wrap, 0, 0);
    lv_obj_set_style_pad_all(pad_wrap, 0, 0);
    lv_obj_clear_flag(pad_wrap, LV_OBJ_FLAG_SCROLLABLE);

    s_pad = lv_obj_create(pad_wrap);
    lv_obj_set_size(s_pad, PAD_SIZE, PAD_SIZE);
    lv_obj_set_style_radius(s_pad, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(s_pad, UI_COLOR_CARD, 0);
    lv_obj_set_style_border_color(s_pad, UI_COLOR_SEP, 0);
    lv_obj_set_style_pad_all(s_pad, 0, 0);
    lv_obj_clear_flag(s_pad, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(s_pad, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(s_pad, joy_cb, LV_EVENT_PRESSED,    NULL);
    lv_obj_add_event_cb(s_pad, joy_cb, LV_EVENT_PRESSING,   NULL);
    lv_obj_add_event_cb(s_pad, joy_cb, LV_EVENT_RELEASED,   NULL);
    lv_obj_add_event_cb(s_pad, joy_cb, LV_EVENT_PRESS_LOST, NULL);

    s_knob = lv_obj_create(s_pad);
    lv_obj_set_size(s_knob, KNOB_SIZE, KNOB_SIZE);
    lv_obj_set_style_radius(s_knob, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(s_knob, UI_COLOR_ACCENT, 0);
    lv_obj_set_style_border_width(s_knob, 0, 0);
    lv_obj_clear_flag(s_knob, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(s_knob, LV_OBJ_FLAG_IGNORE_LAYOUT);   /* 靠 translate 挪 */
    lv_obj_center(s_knob);

    /* ---- 右：一张白卡片装状态和动作，最下面是关模块的行 ----
     * 和工作模式页一个样子：卡片分组 + 最后一行是危险色动作。 */
    lv_obj_t *col = lv_obj_create(row);
    lv_obj_set_size(col, COL_W, LV_PCT(100));
    lv_obj_set_style_bg_opa(col, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(col, 0, 0);
    lv_obj_set_style_pad_all(col, 0, 0);
    lv_obj_set_flex_flow(col, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(col, 6, 0);
    lv_obj_clear_flag(col, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *card = ui_card_create(col);
    lv_obj_set_size(card, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_style_pad_all(card, 6, 0);
    lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(card, 5, 0);
    lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);

    /* 状态和地址两行纯文字（不进 link_row：那两行不需要左右分列的排版） */
    s_lbl_link = lv_label_create(card);
    lv_obj_set_style_text_font(s_lbl_link, UI_FONT_SMALL, 0);
    lv_label_set_text(s_lbl_link, "--");

    s_lbl_ip = lv_label_create(card);
    lv_obj_set_style_text_font(s_lbl_ip, UI_FONT_SMALL, 0);
    lv_obj_set_style_text_color(s_lbl_ip, UI_COLOR_TEXT, 0);
    lv_label_set_text(s_lbl_ip, "--");
    /* 地址行点开是二维码 */
    lv_obj_add_flag(s_lbl_ip, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_ext_click_area(s_lbl_ip, 10);
    lv_obj_add_event_cb(s_lbl_ip, qr_row_cb, LV_EVENT_CLICKED, NULL);

    /* 停车做成整行红块，不是小圆按钮：这是出事时唯一要按到的东西，
     * 得一眼看见、一巴掌拍得到 */
    page_button(card, "STOP", UI_COLOR_DANGER, lv_color_white(), UI_FONT_BODY,
                STOP_H, stop_cb);

    /*
     * 标签都是按"用户看不懂就重写"改过的：
     *   "Web page" + 值 "QR"  → **"Show QR code"**（值留空）
     *     —— 原来那个样子没人看得懂是干嘛的（值写个 "QR" 更糟）
     *   "Send Wi-Fi" + "Not sent" → **"Wi-Fi" + "Not sent"/"Sent"**
     *     —— 宽度：77.9 + 60.8 = 138.7 > 卡片内可用 132，会叠字；
     *        "Wi-Fi"(37.8) + "Not sent"(60.8) = 98.6 ✓
     */
    link_row(card, "Show QR code", "", NULL, qr_row_cb);

    s_row_wifi = link_row(card, "Wi-Fi", "--", &s_val_wifi, wifi_cb);

    /*
     * 关模块单独一行（卡片外面），和工作模式页底部那些动作行一致。
     * **不能再用 ui_list_row_create**：那个标签是 16 号字，
     * "Disable module" 量出来 127.8px，而这栏只有 144px、去掉左右内边距
     * 就剩 120px——照样叠字。这里用 14 号的紧凑行：111.8 ✓
     */
    lv_obj_t *off_row = link_row(col, "Disable module", "", NULL, disable_cb);
    if (off_row != NULL) {
        lv_obj_set_style_text_color(lv_obj_get_child(off_row, 0), UI_COLOR_DANGER, 0);
    }

    tank_refresh();
    s_timer = lv_timer_create(timer_cb, 200, NULL);

    if (!app_tank_is_active()) {
        ui_toast_show("Module not running", UI_COLOR_DANGER);
    }

    ESP_LOGI(TAG, "tank page created (active=%d)", (int)app_tank_is_active());
    return shell;
}

void page_mod_tank_destroy(void)
{
    if (s_timer != NULL) {
        lv_timer_del(s_timer);
        s_timer = NULL;
    }

    /* 二维码浮层挂在 lv_layer_top() 上，**不受页面销毁的管辖**：
     * 不主动收掉的话，离开这一页之后它还在，而且缓冲也漏了 */
    qr_close();

    /* **把车停下，但不停模块**：心跳继续发（20Hz 那条 0,0），链接不断，
     * 用户回头再进这一页还是活的。车要是照最后一条向量一直跑，
     * 那就是"离开页面"变成"失控" */
    app_tank_set_vector(0.0f, 0.0f);

    s_pad = s_knob = NULL;
    s_lbl_link = s_lbl_ip = NULL;
    s_row_wifi = s_val_wifi = NULL;
}
