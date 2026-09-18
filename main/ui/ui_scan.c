/*
 * ui_scan.c — WiFi 扫描选网层实现
 *
 * 生命周期跟 ui_input.c 一个套路：整层建在 lv_layer_top() 上，关闭用
 * lv_obj_del_async（在事件回调里当场删自己会踩 LVGL 的坑）。
 *
 * 扫描在 app_wifi 的 worker 任务里跑，结果回到 scan_done_cb 时**不在**
 * LVGL 上下文，所以那里只做一次 malloc + 拷贝，再 lv_async_call 回
 * LVGL 去建控件。
 */
#include "ui_scan.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "esp_log.h"

#include "app_wifi.h"
#include "lvgl.h"
#include "ui_theme.h"

static const char *TAG = "ui_scan";

/*---------------------------------------------------------------------------
 * 单实例状态
 *-------------------------------------------------------------------------*/
typedef struct {
    int          count;
    esp_err_t    err;
    app_wifi_ap_t aps[APP_WIFI_SCAN_MAX];
} scan_result_t;

static lv_obj_t   *s_root = NULL;       /* 整层根对象；非 NULL 即"开着" */
static lv_obj_t   *s_list = NULL;       /* 列表容器（外壳的内容区） */
static scan_result_t *s_result = NULL;  /* 当前列表对应的结果，行回调靠它取 SSID */
static ui_scan_pick_cb_t s_cb = NULL;
static void       *s_user = NULL;
static bool        s_finished = false;  /* 防止连点选中两次 */

/*---------------------------------------------------------------------------
 * 选中派发：跟 ui_input.c 一样，先 lv_async_call 到下一次 LVGL 处理再调
 * 调用者，这样整层（含正在派发事件的那个行）已经被删干净了
 *-------------------------------------------------------------------------*/
typedef struct {
    char    ssid[33];
    bool    needs_pass;
    ui_scan_pick_cb_t cb;
    void   *user;
} pick_req_t;

static void pick_dispatch(void *arg)
{
    pick_req_t *r = (pick_req_t *)arg;
    if (r->cb != NULL) {
        r->cb(r->ssid, r->needs_pass, r->user);
    }
    free(r);
}

static void close_locked(void)
{
    if (s_root != NULL) {
        /* 不能当场 lv_obj_del：现在多半正在这个控件树的事件回调里 */
        lv_obj_del_async(s_root);
        s_root = NULL;
    }
    s_list = NULL;
    free(s_result);
    s_result = NULL;
    s_cb = NULL;
    s_user = NULL;
}

static void finish(const char *ssid, bool needs_pass)
{
    if (s_finished) {
        return;
    }
    s_finished = true;

    pick_req_t *req = malloc(sizeof(*req));
    if (req != NULL) {
        strlcpy(req->ssid, ssid ? ssid : "", sizeof(req->ssid));
        req->needs_pass = needs_pass;
        req->cb = s_cb;
        req->user = s_user;
    }
    close_locked();

    if (req == NULL) {
        return;
    }
    if (req->cb != NULL) {
        lv_async_call(pick_dispatch, req);
    } else {
        free(req);
    }
}

/* rescan_cb 要用，定义在后面 */
static void scan_done_cb(const app_wifi_ap_t *aps, int count, esp_err_t err);

/*---------------------------------------------------------------------------
 * 行
 *-------------------------------------------------------------------------*/
/*
 * ui_list_row_create 的名字 label 不限宽，32 字符的 SSID 会直接压到右侧
 * 的信号值上。它是这一行的第 0 个子对象，取出来限宽 + 超长打省略号。
 */
static void limit_row_name(lv_obj_t *row, lv_coord_t w)
{
    lv_obj_t *name = lv_obj_get_child(row, 0);
    if (name != NULL) {
        lv_obj_set_width(name, w);
        lv_label_set_long_mode(name, LV_LABEL_LONG_DOT);
    }
}

static void ap_clicked_cb(lv_event_t *e)
{
    int idx = (int)(intptr_t)lv_event_get_user_data(e);
    if (s_result == NULL || idx < 0 || idx >= s_result->count) {
        return;
    }
    finish(s_result->aps[idx].ssid, s_result->aps[idx].needs_pass);
}

/* 真正的重扫动作。必须延后到事件回调之外执行：它要 lv_obj_clean 掉整个
 * 列表，而"Scan Again"那一行就在这个列表里，事件还在派发中就把它删了
 * 是 LVGL 里未定义的行为（见 ui_theme.c 里返回键那段注释）。 */
static void rescan_async(void *arg)
{
    (void)arg;
    if (s_root == NULL || s_list == NULL) {
        return;                 /* 延后执行期间用户已经把整层关了 */
    }
    lv_obj_clean(s_list);
    free(s_result);
    s_result = NULL;

    lv_obj_t *lbl = lv_label_create(s_list);
    lv_label_set_text(lbl, "Scanning...");
    lv_obj_set_style_text_font(lbl, UI_FONT_SMALL, 0);
    lv_obj_set_style_text_color(lbl, UI_COLOR_TEXT_SUB, 0);
    app_wifi_scan(scan_done_cb);
}

static void rescan_cb(lv_event_t *e)
{
    (void)e;
    lv_async_call(rescan_async, NULL);
}

static void add_tappable_row(const char *label, const char *value,
                             bool arrow, lv_event_cb_t cb, void *user)
{
    lv_obj_t *row = ui_list_row_create(s_list, label, value, arrow);
    lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(row, cb, LV_EVENT_CLICKED, user);
    limit_row_name(row, 168);
}

static void add_plain_row(const char *text)
{
    lv_obj_t *lbl = lv_label_create(s_list);
    lv_label_set_text(lbl, text);
    lv_obj_set_style_text_font(lbl, UI_FONT_SMALL, 0);
    lv_obj_set_style_text_color(lbl, UI_COLOR_TEXT_SUB, 0);
    lv_obj_set_style_pad_top(lbl, 8, 0);
}

/*---------------------------------------------------------------------------
 * 扫描结果 → 列表（已回到 LVGL 上下文）
 *-------------------------------------------------------------------------*/
static void scan_result_async(void *arg)
{
    scan_result_t *r = (scan_result_t *)arg;

    /* 扫的时候用户可能已经把这一层关了。结果必须 free，但不能再建控件 */
    if (s_root == NULL || s_list == NULL) {
        free(r);
        return;
    }

    lv_obj_clean(s_list);
    free(s_result);
    s_result = r;

    if (r->err != ESP_OK) {
        ESP_LOGE(TAG, "scan error: %s", esp_err_to_name(r->err));
        add_plain_row("Scan failed");
        add_tappable_row("Scan Again", "", false, rescan_cb, NULL);
        return;
    }
    if (r->count == 0) {
        add_plain_row("No networks found");
        add_tappable_row("Scan Again", "", false, rescan_cb, NULL);
        return;
    }

    ESP_LOGI(TAG, "listing %d network(s)", r->count);
    for (int i = 0; i < r->count; i++) {
        char value[16];
        snprintf(value, sizeof(value), "%d dBm", (int)r->aps[i].rssi);
        /* 需要密码的显示箭头——montserrat 字体里没有锁形符号，
         * 用"要不要再输一步"当安全标识 */
        add_tappable_row(r->aps[i].ssid, value, r->aps[i].needs_pass,
                         ap_clicked_cb, (void *)(intptr_t)i);
    }
    add_tappable_row("Scan Again", "", false, rescan_cb, NULL);
}

static void scan_done_cb(const app_wifi_ap_t *aps, int count, esp_err_t err)
{
    /* 这里在 app_wifi 的扫描任务上下文，不能碰控件 */
    scan_result_t *r = malloc(sizeof(*r));
    if (r == NULL) {
        ESP_LOGE(TAG, "no mem for scan result");
        return;
    }
    r->err = err;
    r->count = (count > APP_WIFI_SCAN_MAX) ? APP_WIFI_SCAN_MAX : count;
    if (aps != NULL && r->count > 0) {
        memcpy(r->aps, aps, (size_t)r->count * sizeof(r->aps[0]));
    } else {
        r->count = 0;
    }
    lv_async_call(scan_result_async, r);
}

/*---------------------------------------------------------------------------
 * 对外 API
 *-------------------------------------------------------------------------*/
static void back_cb(lv_event_t *e)
{
    (void)e;
    close_locked();             /* s_finished 不置位：这只是退出，不是选中 */
}

void ui_scan_open(ui_scan_pick_cb_t on_pick, void *user)
{
    if (s_root != NULL) {
        return;                 /* 已经开着，别叠第二层 */
    }

    s_cb = on_pick;
    s_user = user;
    s_finished = false;

    /* 返回键关自己而不是回主菜单，所以用 _ex 版 */
    s_root = ui_page_shell_create_ex(lv_layer_top(), "Wi-Fi", back_cb);
    s_list = ui_page_shell_content(s_root);

    lv_obj_t *lbl = lv_label_create(s_list);
    lv_label_set_text(lbl, "Scanning...");
    lv_obj_set_style_text_font(lbl, UI_FONT_SMALL, 0);
    lv_obj_set_style_text_color(lbl, UI_COLOR_TEXT_SUB, 0);

    esp_err_t err = app_wifi_scan(scan_done_cb);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        /* 已经在扫（INVALID_STATE）不用管：那次的回调会落到这一层。
         * 其它错误就得说出来了 */
        ESP_LOGE(TAG, "scan start failed: %s", esp_err_to_name(err));
        lv_obj_clean(s_list);
        add_plain_row("Scan failed");
        add_tappable_row("Scan Again", "", false, rescan_cb, NULL);
    }
}

void ui_scan_close(void)
{
    close_locked();
}

bool ui_scan_is_open(void)
{
    return s_root != NULL;
}
