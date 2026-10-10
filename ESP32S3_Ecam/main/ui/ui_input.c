/*
 * ui_input.c — 全屏文本输入层实现
 *
 * 布局（320x240）：标题栏 30 + 输入框 36 + 键盘 170。键盘用 14 号字体，
 * 320 宽放 12 个键每键约 25px，再大就点不中了。
 *
 * 删除时机与 page_camera.c 的拍照回调同理：保存/取消都发生在 LVGL 事件
 * 回调里，当场 lv_obj_del 自己会踩 LVGL 的坑，所以用 lv_async_call 把
 * 「回调 + 文本」投到下一次 LVGL 处理，再用 lv_obj_del_async 收掉整层。
 */
#include "ui_input.h"

#include <stdlib.h>
#include <string.h>

#include "lvgl.h"

#include "ui_theme.h"

/*---------------------------------------------------------------------------
 * 单实例状态
 *-------------------------------------------------------------------------*/
static lv_obj_t *s_root = NULL;
static lv_obj_t *s_ta = NULL;
static bool s_password = false;
static bool s_finished = false;         /* 防止保存/取消同时触发 */
static ui_input_done_cb_t s_cb = NULL;
static void *s_user = NULL;

typedef struct {
    char *text;                         /* NULL = 取消 */
    ui_input_done_cb_t cb;
    void *user;
} result_t;

static void finish_dispatch(void *arg);

/*---------------------------------------------------------------------------
 * 收尾
 *-------------------------------------------------------------------------*/
static void finish_locked(const char *text)
{
    if (s_finished) {
        return;
    }
    s_finished = true;

    result_t *r = malloc(sizeof(result_t));
    if (r == NULL) {
        /* 极端情况下丢一次回调，总好过悬空指针 */
        s_root = NULL;
        s_ta = NULL;
        s_cb = NULL;
        s_user = NULL;
        return;
    }
    r->text = text ? strdup(text) : NULL;
    r->cb = s_cb;
    r->user = s_user;

    /* 必须用 async 版：lv_async_call 的 payload 在下一次 LVGL 处理前、
     * 当前事件回调彻底退出后才执行，而 lv_obj_del_async 也会在事件
     * 处理结束后才真正删除控件。 */
    lv_async_call((lv_async_cb_t)finish_dispatch, r);
    lv_obj_del_async(s_root);

    s_root = NULL;
    s_ta = NULL;
    s_cb = NULL;
    s_user = NULL;
}

static void finish_dispatch(void *arg)
{
    result_t *r = (result_t *)arg;
    if (r->cb != NULL) {
        r->cb(r->text, r->user);
    }
    free(r->text);
    free(r);
}

/*---------------------------------------------------------------------------
 * 事件
 *-------------------------------------------------------------------------*/
static void save_cb(lv_event_t *e)
{
    (void)e;
    finish_locked(lv_textarea_get_text(s_ta));
}

static void cancel_cb(lv_event_t *e)
{
    (void)e;
    finish_locked(NULL);
}

static void kb_ready_cb(lv_event_t *e)
{
    (void)e;
    finish_locked(lv_textarea_get_text(s_ta));
}

static void kb_cancel_cb(lv_event_t *e)
{
    (void)e;
    finish_locked(NULL);
}

static void eye_cb(lv_event_t *e)
{
    lv_obj_t *btn = lv_event_get_target(e);
    lv_label_set_text(lv_obj_get_child(btn, 0),
                      s_password ? LV_SYMBOL_EYE_CLOSE : LV_SYMBOL_EYE_OPEN);
    s_password = !s_password;
    lv_textarea_set_password_mode(s_ta, s_password);
}

/*---------------------------------------------------------------------------
 * 对外 API
 *-------------------------------------------------------------------------*/
void ui_input_open(const char *title, const char *value,
                   bool password, bool numeric, size_t max_len,
                   ui_input_done_cb_t cb, void *user)
{
    /* 一层没关就再开一层会互相踩状态，直接忽略 */
    if (s_root != NULL) {
        return;
    }

    s_password = password;
    s_finished = false;
    s_cb = cb;
    s_user = user;

    /* 全屏根层：白色背景盖住下面的页面 */
    s_root = lv_obj_create(lv_layer_top());
    lv_obj_set_size(s_root, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_color(s_root, UI_COLOR_CARD, 0);
    lv_obj_set_style_bg_opa(s_root, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(s_root, 0, 0);
    lv_obj_set_style_radius(s_root, 0, 0);
    lv_obj_set_style_pad_all(s_root, 0, 0);
    lv_obj_clear_flag(s_root, LV_OBJ_FLAG_SCROLLABLE);

    /* ---- 标题栏 ---- */
    lv_obj_t *bar = lv_obj_create(s_root);
    lv_obj_set_size(bar, LV_PCT(100), 30);
    lv_obj_align(bar, LV_ALIGN_TOP_MID, 0, 0);
    lv_obj_set_style_bg_color(bar, UI_COLOR_BAR, 0);
    lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(bar, 0, 0);
    lv_obj_set_style_border_side(bar, LV_BORDER_SIDE_BOTTOM, 0);
    lv_obj_set_style_border_width(bar, 1, 0);
    lv_obj_set_style_border_color(bar, UI_COLOR_SEP, 0);
    lv_obj_set_style_radius(bar, 0, 0);
    lv_obj_set_style_pad_all(bar, 0, 0);
    lv_obj_clear_flag(bar, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *cancel = lv_btn_create(bar);
    lv_obj_set_size(cancel, 56, 24);
    lv_obj_align(cancel, LV_ALIGN_LEFT_MID, 4, 0);
    lv_obj_set_style_bg_opa(cancel, LV_OPA_TRANSP, 0);
    lv_obj_set_style_shadow_width(cancel, 0, 0);
    lv_obj_add_event_cb(cancel, cancel_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *cancel_lbl = lv_label_create(cancel);
    lv_label_set_text(cancel_lbl, "Cancel");
    lv_obj_set_style_text_font(cancel_lbl, UI_FONT_SMALL, 0);
    lv_obj_set_style_text_color(cancel_lbl, UI_COLOR_ACCENT, 0);
    lv_obj_center(cancel_lbl);

    lv_obj_t *save = lv_btn_create(bar);
    lv_obj_set_size(save, 48, 24);
    lv_obj_align(save, LV_ALIGN_RIGHT_MID, -4, 0);
    lv_obj_set_style_bg_opa(save, LV_OPA_TRANSP, 0);
    lv_obj_set_style_shadow_width(save, 0, 0);
    lv_obj_add_event_cb(save, save_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *save_lbl = lv_label_create(save);
    lv_label_set_text(save_lbl, "Save");
    lv_obj_set_style_text_font(save_lbl, UI_FONT_SMALL, 0);
    lv_obj_set_style_text_color(save_lbl, UI_COLOR_ACCENT, 0);
    lv_obj_center(save_lbl);

    if (password) {
        /* 明文切换眼：Save 左边 */
        lv_obj_t *eye = lv_btn_create(bar);
        lv_obj_set_size(eye, 30, 24);
        lv_obj_align(eye, LV_ALIGN_RIGHT_MID, -56, 0);
        lv_obj_set_style_bg_opa(eye, LV_OPA_TRANSP, 0);
        lv_obj_set_style_shadow_width(eye, 0, 0);
        lv_obj_add_event_cb(eye, eye_cb, LV_EVENT_CLICKED, NULL);
        lv_obj_t *eye_lbl = lv_label_create(eye);
        lv_label_set_text(eye_lbl, LV_SYMBOL_EYE_OPEN);
        lv_obj_set_style_text_font(eye_lbl, UI_FONT_BODY, 0);
        lv_obj_set_style_text_color(eye_lbl, UI_COLOR_TEXT_SUB, 0);
        lv_obj_center(eye_lbl);
    }

    lv_obj_t *title_lbl = lv_label_create(bar);
    lv_label_set_text(title_lbl, title ? title : "");
    lv_obj_set_style_text_font(title_lbl, UI_FONT_BODY, 0);
    lv_obj_set_style_text_color(title_lbl, UI_COLOR_TEXT, 0);
    lv_obj_align(title_lbl, LV_ALIGN_CENTER, 0, 0);

    /* ---- 输入框 ---- */
    s_ta = lv_textarea_create(s_root);
    lv_obj_set_size(s_ta, LV_PCT(100) - 20, 34);
    lv_obj_align(s_ta, LV_ALIGN_TOP_MID, 0, 34);
    lv_textarea_set_one_line(s_ta, true);
    lv_textarea_set_max_length(s_ta, (uint32_t)max_len);
    lv_textarea_set_text(s_ta, value ? value : "");
    lv_textarea_set_password_mode(s_ta, password);
    lv_obj_set_style_text_font(s_ta, UI_FONT_BODY, 0);
    lv_obj_set_style_text_color(s_ta, UI_COLOR_TEXT, 0);
    lv_obj_set_style_bg_color(s_ta, UI_COLOR_BG, 0);
    lv_obj_set_style_bg_opa(s_ta, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(s_ta, UI_COLOR_SEP, 0);
    lv_obj_set_style_border_width(s_ta, 1, 0);
    lv_obj_set_style_radius(s_ta, 8, 0);
    lv_obj_set_style_pad_all(s_ta, 6, 0);

    /* ---- 软键盘 ---- */
    lv_obj_t *kb = lv_keyboard_create(s_root);
    lv_obj_set_size(kb, LV_PCT(100), 172);
    lv_obj_align(kb, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_keyboard_set_textarea(kb, s_ta);
    if (numeric) {
        lv_keyboard_set_mode(kb, LV_KEYBOARD_MODE_NUMBER);
    }
    /* 键帽字体：14 号。默认 14 是 LVGL 默认字体，本来就够小；
     * 这里统一主题里的 SMALL 以保持一致。 */
    lv_obj_set_style_text_font(kb, UI_FONT_SMALL, 0);
    lv_obj_set_style_text_font(kb, UI_FONT_SMALL, LV_PART_ITEMS);
    lv_obj_add_event_cb(kb, kb_ready_cb, LV_EVENT_READY, NULL);
    lv_obj_add_event_cb(kb, kb_cancel_cb, LV_EVENT_CANCEL, NULL);
}
