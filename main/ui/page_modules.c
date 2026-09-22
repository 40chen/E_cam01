/*
 * page_modules.c — 外接模块页
 *
 * 列出插在模块口（GPIO10/11）上的外设。检测在 app_module 里（板子只有
 * 这两个脚是空的，检测靠模块侧的下拉电阻，见 README 的"外接模块"一节），
 * 这里只负责显示和跳转。
 *
 * **当前是空态**：检测和 WS2812 氛围灯还没实现，页面先占住主菜单右上角
 * 那个格子的落点，免得 3x2 布局里少一张卡。
 */
#include "page_modules.h"

#include "esp_log.h"

#include "ui.h"
#include "ui_theme.h"

static const char *TAG = "page_modules";

lv_obj_t *page_modules_create(lv_obj_t *parent)
{
    lv_obj_t *shell = ui_page_shell_create(parent, "Modules");
    lv_obj_t *content = ui_page_shell_content(shell);
    lv_obj_set_flex_align(content, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);

    lv_obj_t *icon = lv_label_create(content);
    lv_label_set_text(icon, LV_SYMBOL_TINT);
    lv_obj_set_style_text_font(icon, UI_FONT_ICON, 0);
    lv_obj_set_style_text_color(icon, UI_COLOR_TEXT_SUB, 0);

    lv_obj_t *line1 = lv_label_create(content);
    lv_label_set_text(line1, "No module detected");
    lv_obj_set_style_text_font(line1, UI_FONT_BODY, 0);
    lv_obj_set_style_text_color(line1, UI_COLOR_TEXT, 0);

    lv_obj_t *line2 = lv_label_create(content);
    lv_label_set_text(line2, "Plug a module into the\nmodule port to see it here.");
    lv_obj_set_style_text_align(line2, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_font(line2, UI_FONT_SMALL, 0);
    lv_obj_set_style_text_color(line2, UI_COLOR_TEXT_SUB, 0);

    ESP_LOGI(TAG, "modules page created (empty state)");
    return shell;
}

void page_modules_destroy(void)
{
}
