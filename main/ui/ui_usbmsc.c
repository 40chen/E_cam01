/*
 * ui_usbmsc.c — U 盘模式的界面
 *
 * 一页提示 + 一个 Exit 按钮。**提示里那句"先在电脑上安全弹出"是必须的**：
 * 电脑还在写的时候退出，那边可能还有没落盘的数据，这是这个功能唯一会让
 * 用户丢数据的操作，代码挡不住（我们没法强制电脑刷新缓存），只能讲清楚。
 *
 * 浮层建在 lv_layer_top() 上（和音乐页的大图模式、ui_scan 一个做法）：
 * 设置页还活着，关掉浮层就回到它。
 */
#include "ui_usbmsc.h"

#include "esp_log.h"

#include "app_usbmsc.h"
#include "app_storage.h"
#include "ui.h"
#include "ui_theme.h"

static const char *TAG = "ui_usbmsc";

static lv_obj_t *s_root;

static void do_exit(void)
{
    esp_err_t err = app_usbmsc_exit();
    if (err != ESP_OK) {
        char buf[48];
        snprintf(buf, sizeof(buf), "Remount failed: %s", esp_err_to_name(err));
        ui_toast_show(buf, UI_COLOR_DANGER);
    } else {
        ui_toast_show("TF card back to the device", lv_color_white());
    }
}

static void close_overlay(void)
{
    if (s_root == NULL) {
        return;
    }
    lv_obj_t *obj = s_root;
    s_root = NULL;                      /* 先清指针再删：删除是异步的 */
    lv_obj_del_async(obj);
}

static void exit_clicked_cb(lv_event_t *e)
{
    (void)e;
    do_exit();
    close_overlay();
}

static void back_cb(lv_event_t *e)
{
    (void)e;
    /* 返回键等同于 Exit：这一页没有别的去处，而且不留着模式开着偷偷跑 */
    do_exit();
    close_overlay();
}

void ui_usbmsc_open(void)
{
    if (s_root != NULL) {
        return;                         /* 已经开着 */
    }

    esp_err_t err = app_usbmsc_enter();
    if (err != ESP_OK) {
        char buf[48];
        if (err == ESP_ERR_INVALID_STATE) {
            snprintf(buf, sizeof(buf), "No TF card");
        } else {
            snprintf(buf, sizeof(buf), "USB mode failed: %s", esp_err_to_name(err));
        }
        ui_toast_show(buf, UI_COLOR_DANGER);
        return;
    }

    s_root = ui_page_shell_create_ex(lv_layer_top(), "USB Drive", back_cb);
    lv_obj_t *content = ui_page_shell_content(s_root);
    lv_obj_set_flex_align(content, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);

    lv_obj_t *icon = lv_label_create(content);
    lv_label_set_text(icon, LV_SYMBOL_USB);
    lv_obj_set_style_text_font(icon, UI_FONT_ICON, 0);
    lv_obj_set_style_text_color(icon, UI_COLOR_ACCENT, 0);

    lv_obj_t *line1 = lv_label_create(content);
    lv_label_set_text(line1, "The TF card is now\nshared with your computer.");
    lv_obj_set_style_text_align(line1, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_font(line1, UI_FONT_BODY, 0);
    lv_obj_set_style_text_color(line1, UI_COLOR_TEXT, 0);

    lv_obj_t *line2 = lv_label_create(content);
    lv_label_set_text(line2, "Eject it on the PC first,\nthen tap Exit.");
    lv_obj_set_style_text_align(line2, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_font(line2, UI_FONT_SMALL, 0);
    lv_obj_set_style_text_color(line2, UI_COLOR_TEXT_SUB, 0);

    lv_obj_t *btn = lv_btn_create(content);
    lv_obj_set_size(btn, 120, 40);
    lv_obj_set_style_bg_color(btn, UI_COLOR_ACCENT, 0);
    lv_obj_set_style_radius(btn, UI_RADIUS_BTN, 0);
    lv_obj_set_style_shadow_width(btn, 0, 0);
    lv_obj_set_ext_click_area(btn, 8);
    lv_obj_add_event_cb(btn, exit_clicked_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *bl = lv_label_create(btn);
    lv_label_set_text(bl, "Exit");
    lv_obj_set_style_text_font(bl, UI_FONT_BODY, 0);
    lv_obj_set_style_text_color(bl, lv_color_white(), 0);
    lv_obj_center(bl);

    ESP_LOGI(TAG, "U 盘模式界面已打开");
}
