/*
 * page_home.c — 图标主菜单（开机页）
 *
 * 2x2 图标网格：Camera / Music / Photos / Settings。
 * 点任一格进入对应全屏子页，子页左上角返回箭头回到这里。
 */
#include <stdio.h>

#include "app_storage.h"
#include "esp_log.h"
#include "ui.h"
#include "ui_theme.h"

#define CELL_W   146
#define CELL_H   92
#define GRID_GAP 8

typedef struct {
    const char *icon;
    const char *label;
    void (*open)(void);
} menu_item_t;

static void on_camera(void)   { ui_goto_async(UI_PAGE_CAMERA); }
static void on_music(void)    { ui_open_music(); }
static void on_photos(void)   { ui_open_photos(); }
static void on_settings(void) { ui_open_settings(); }

static const menu_item_t s_items[4] = {
    { LV_SYMBOL_IMAGE,     "Camera",   on_camera },
    { LV_SYMBOL_AUDIO,     "Music",    on_music },
    { LV_SYMBOL_DIRECTORY, "Photos",   on_photos },
    { LV_SYMBOL_SETTINGS,  "Settings", on_settings },
};

static lv_obj_t *s_sd_label = NULL;

static void menu_cell_cb(lv_event_t *e)
{
    const menu_item_t *item = (const menu_item_t *)lv_event_get_user_data(e);
    if (item != NULL && item->open != NULL) {
        item->open();
    }
}

static lv_obj_t *create_cell(lv_obj_t *parent, const menu_item_t *item)
{
    lv_obj_t *cell = lv_btn_create(parent);
    lv_obj_set_size(cell, CELL_W, CELL_H);
    lv_obj_set_style_bg_color(cell, UI_COLOR_CARD, 0);
    lv_obj_set_style_bg_opa(cell, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(cell, UI_RADIUS_CARD, 0);
    lv_obj_set_style_shadow_width(cell, 8, 0);
    lv_obj_set_style_shadow_opa(cell, LV_OPA_10, 0);
    lv_obj_set_style_shadow_ofs_y(cell, 2, 0);
    lv_obj_set_style_shadow_color(cell, lv_color_black(), 0);
    /* 按下时的轻微缩放，接近 iOS 的手感 */
    lv_obj_set_style_transform_zoom(cell, 245, LV_STATE_PRESSED);
    lv_obj_add_event_cb(cell, menu_cell_cb, LV_EVENT_CLICKED, (void *)item);

    lv_obj_t *icon = lv_label_create(cell);
    lv_label_set_text(icon, item->icon);
    lv_obj_set_style_text_font(icon, UI_FONT_ICON, 0);
    lv_obj_set_style_text_color(icon, UI_COLOR_ACCENT, 0);
    lv_obj_align(icon, LV_ALIGN_TOP_MID, 0, 6);

    lv_obj_t *label = lv_label_create(cell);
    lv_label_set_text(label, item->label);
    lv_obj_set_style_text_font(label, UI_FONT_BODY, 0);
    lv_obj_set_style_text_color(label, UI_COLOR_TEXT, 0);
    lv_obj_align(label, LV_ALIGN_BOTTOM_MID, 0, -8);

    return cell;
}

static void refresh_status(void)
{
    if (s_sd_label == NULL) {
        return;
    }
    lv_label_set_text(s_sd_label,
                      app_storage_ready() ? LV_SYMBOL_SD_CARD : LV_SYMBOL_WARNING);
    lv_obj_set_style_text_color(s_sd_label,
                                app_storage_ready() ? UI_COLOR_ACCENT : UI_COLOR_DANGER, 0);
}

static void status_timer_cb(lv_timer_t *t)
{
    (void)t;
    refresh_status();
}

static lv_timer_t *s_status_timer = NULL;

lv_obj_t *page_home_create(lv_obj_t *parent)
{
    lv_obj_t *root = lv_obj_create(parent);
    lv_obj_set_size(root, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_opa(root, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(root, 0, 0);
    lv_obj_set_style_radius(root, 0, 0);
    lv_obj_set_style_pad_all(root, 10, 0);
    lv_obj_set_style_pad_row(root, 6, 0);
    lv_obj_clear_flag(root, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(root, LV_FLEX_FLOW_COLUMN);

    /* 顶部状态条：标题 + TF 卡状态 */
    lv_obj_t *bar = lv_obj_create(root);
    lv_obj_set_size(bar, LV_PCT(100), 22);
    lv_obj_set_style_bg_opa(bar, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(bar, 0, 0);
    lv_obj_set_style_pad_all(bar, 0, 0);
    lv_obj_clear_flag(bar, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *title = lv_label_create(bar);
    lv_label_set_text(title, "ERDCX Camera");
    lv_obj_set_style_text_font(title, UI_FONT_TITLE, 0);
    lv_obj_set_style_text_color(title, UI_COLOR_TEXT, 0);
    lv_obj_align(title, LV_ALIGN_LEFT_MID, 2, 0);

    s_sd_label = lv_label_create(bar);
    lv_obj_set_style_text_font(s_sd_label, UI_FONT_BODY, 0);
    lv_obj_align(s_sd_label, LV_ALIGN_RIGHT_MID, -2, 0);
    refresh_status();

    /* 2x2 网格：用 flex_grow 吃掉状态条以下的全部高度 */
    lv_obj_t *grid = lv_obj_create(root);
    lv_obj_set_width(grid, LV_PCT(100));
    lv_obj_set_flex_grow(grid, 1);
    lv_obj_set_style_bg_opa(grid, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(grid, 0, 0);
    lv_obj_set_style_pad_all(grid, 0, 0);
    lv_obj_set_style_pad_column(grid, GRID_GAP, 0);
    lv_obj_set_style_pad_row(grid, GRID_GAP, 0);
    lv_obj_clear_flag(grid, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(grid, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_flex_align(grid, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_START);

    for (size_t i = 0; i < sizeof(s_items) / sizeof(s_items[0]); i++) {
        create_cell(grid, &s_items[i]);
    }

    s_status_timer = lv_timer_create(status_timer_cb, 2000, NULL);
    return root;
}

void page_home_destroy(void)
{
    if (s_status_timer != NULL) {
        lv_timer_del(s_status_timer);
        s_status_timer = NULL;
    }
    s_sd_label = NULL;
}
