/*
 * ui_theme.h — 界面样式常量与公共控件
 *
 * 参考苹果设计语言：大圆角、卡片、充足留白、克制的配色。
 * 屏幕只有 320x240，320x240 的实时高斯模糊不现实，所以"半透明"一律用
 * 半透明纯色实现。
 *
 * 界面文案目前一律使用英文/图标（LVGL 内置字体不含中文字库）。
 * 日后要上中文，需要先生成只含界面用字的子集字体。
 */
#pragma once

#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

/*---------------------------------------------------------------------------
 * 配色
 *-------------------------------------------------------------------------*/
#define UI_COLOR_BG          lv_color_hex(0xF2F2F7)   /* iOS 分组背景灰 */
#define UI_COLOR_CARD        lv_color_hex(0xFFFFFF)
#define UI_COLOR_ACCENT      lv_color_hex(0x007AFF)   /* iOS 蓝 */
#define UI_COLOR_TEXT        lv_color_hex(0x1C1C1E)
#define UI_COLOR_TEXT_SUB    lv_color_hex(0x8E8E93)
#define UI_COLOR_SEP         lv_color_hex(0xC6C6C8)
#define UI_COLOR_DANGER      lv_color_hex(0xFF3B30)
#define UI_COLOR_BAR         lv_color_hex(0xF9F9F9)

/*---------------------------------------------------------------------------
 * 尺寸
 *-------------------------------------------------------------------------*/
#define UI_SCREEN_W          320
#define UI_SCREEN_H          240
#define UI_RADIUS_CARD       12
#define UI_RADIUS_BTN        10
#define UI_TITLE_BAR_H       34
#define UI_PAD               8

/*---------------------------------------------------------------------------
 * 字体
 *-------------------------------------------------------------------------*/
#define UI_FONT_SMALL   (&lv_font_montserrat_14)
#define UI_FONT_BODY    (&lv_font_montserrat_16)
#define UI_FONT_TITLE   (&lv_font_montserrat_20)
#define UI_FONT_ICON    (&lv_font_montserrat_28)

/*---------------------------------------------------------------------------
 * 公共控件
 *-------------------------------------------------------------------------*/

/* 给整屏铺上主题背景色 */
void ui_theme_apply_screen(lv_obj_t *scr);

/*
 * 建一个标准的"子页外壳"：主题背景 + 顶部标题栏（含返回按钮）+ 内容区。
 * 返回外壳根对象（页面模块必须把它原样 return 出去，切页时销毁的就是它）；
 * 往里塞内容用 ui_page_shell_content()。
 * 返回按钮点击后自动回到主菜单。
 */
lv_obj_t *ui_page_shell_create(lv_obj_t *parent, const char *title);

/*
 * 同上，但返回键的行为由调用者决定。普通页面用上面那个就够了（返回键
 * 回主菜单）；不是页面的东西（比如盖在设置页上的扫描层）需要让返回键
 * 关掉自己，就用这个。
 */
lv_obj_t *ui_page_shell_create_ex(lv_obj_t *parent, const char *title,
                                  lv_event_cb_t back_cb);

/* 取外壳里的内容区（ui_page_shell_create 的返回值） */
lv_obj_t *ui_page_shell_content(lv_obj_t *shell);

/* 一张 iOS 风格的白色圆角卡片 */
lv_obj_t *ui_card_create(lv_obj_t *parent);

/* 设置列表里的一行：左侧标题，右侧可选的值文本与箭头。
 * 返回整行对象，可对其挂 LV_EVENT_CLICKED。 */
lv_obj_t *ui_list_row_create(lv_obj_t *parent, const char *label,
                             const char *value, bool show_arrow);

/* 更新某一行右侧的值文本（ui_list_row_create 的返回值） */
void ui_list_row_set_value(lv_obj_t *row, const char *value);

/* 圆形按钮（如快门） */
lv_obj_t *ui_round_button_create(lv_obj_t *parent, int diameter, lv_color_t color);

/*
 * 屏幕上方的黑色半透明提示条，1.8 秒后自动消失。
 * 建在 lv_layer_top() 上而不是页面里：设置页的内容区是滚动的，塞进去
 * 会跟着滚、还会被裁掉。必须在 LVGL 上下文里调用（事件回调或
 * lv_async_call 里），不能从 worker 任务直接调。
 */
void ui_toast_show(const char *text, lv_color_t color);

#ifdef __cplusplus
}
#endif
