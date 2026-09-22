#include "ui_theme.h"

#include <string.h>

#include "ui.h"

void ui_theme_apply_screen(lv_obj_t *scr)
{
    lv_obj_set_style_bg_color(scr, UI_COLOR_BG, 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
    lv_obj_clear_flag(scr, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_pad_all(scr, 0, 0);
}

lv_obj_t *ui_card_create(lv_obj_t *parent)
{
    lv_obj_t *card = lv_obj_create(parent);
    lv_obj_set_style_bg_color(card, UI_COLOR_CARD, 0);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(card, UI_RADIUS_CARD, 0);
    lv_obj_set_style_border_width(card, 0, 0);
    lv_obj_set_style_pad_all(card, UI_PAD, 0);
    lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);
    /* 轻微投影，模拟 iOS 卡片的层次感 */
    lv_obj_set_style_shadow_width(card, 8, 0);
    lv_obj_set_style_shadow_opa(card, LV_OPA_10, 0);
    lv_obj_set_style_shadow_ofs_y(card, 2, 0);
    lv_obj_set_style_shadow_color(card, lv_color_black(), 0);
    return card;
}

static void back_btn_cb(lv_event_t *e)
{
    (void)e;
    /* 必须用异步版本：回调正在执行时就把整个页面（含这个按钮）删掉，
     * 在 LVGL 里是未定义行为，会 assert 崩溃。 */
    ui_go_home_async();
}

lv_obj_t *ui_page_shell_create(lv_obj_t *parent, const char *title)
{
    return ui_page_shell_create_ex(parent, title, back_btn_cb);
}

lv_obj_t *ui_page_shell_create_ex(lv_obj_t *parent, const char *title,
                                  lv_event_cb_t back_cb)
{
    lv_obj_t *root = lv_obj_create(parent);
    lv_obj_set_size(root, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_color(root, UI_COLOR_BG, 0);
    lv_obj_set_style_bg_opa(root, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(root, 0, 0);
    lv_obj_set_style_radius(root, 0, 0);
    lv_obj_set_style_pad_all(root, 0, 0);
    lv_obj_set_style_pad_row(root, 0, 0);
    lv_obj_clear_flag(root, LV_OBJ_FLAG_SCROLLABLE);
    /* 用 flex 纵向排布，内容区用 flex_grow 吃掉剩余高度。
     * 不要写 LV_PCT(100) - UI_TITLE_BAR_H：LV_PCT() 返回的是百分比编码值，
     * 减一个常量得到的是"另一个百分比"，不是"百分比减像素"。 */
    lv_obj_set_flex_flow(root, LV_FLEX_FLOW_COLUMN);

    /* 标题栏 */
    lv_obj_t *bar = lv_obj_create(root);
    lv_obj_set_size(bar, LV_PCT(100), UI_TITLE_BAR_H);
    lv_obj_set_style_bg_color(bar, UI_COLOR_BAR, 0);
    lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(bar, 0, 0);
    lv_obj_set_style_border_side(bar, LV_BORDER_SIDE_BOTTOM, 0);
    lv_obj_set_style_border_width(bar, 1, 0);
    lv_obj_set_style_border_color(bar, UI_COLOR_SEP, 0);
    lv_obj_set_style_radius(bar, 0, 0);
    lv_obj_set_style_pad_all(bar, 0, 0);
    lv_obj_clear_flag(bar, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *btn = lv_btn_create(bar);
    /* 46 宽而不是原来的 38：返回键是每个子页唯一的导航出口，做宽一点
     * 更容易点中。标题栏是 34 高，按钮留 6 给上下边距 */
    lv_obj_set_size(btn, 46, UI_TITLE_BAR_H - 8);
    lv_obj_align(btn, LV_ALIGN_LEFT_MID, 4, 0);
    lv_obj_set_style_bg_opa(btn, LV_OPA_TRANSP, 0);
    lv_obj_set_style_shadow_width(btn, 0, 0);
    /* 视觉上 38x28 就够了，但手指点不中：触摸目标一般建议 44x44 以上，
     * 而这个键离屏幕左边缘只有 4px。ext_click_area 只扩大可点区域，
     * 不动视觉尺寸和布局。标题栏里没有别的可点对象，放大不会误触。 */
    lv_obj_set_ext_click_area(btn, 12);
    lv_obj_add_event_cb(btn, back_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *arrow = lv_label_create(btn);
    lv_label_set_text(arrow, LV_SYMBOL_LEFT);
    lv_obj_set_style_text_font(arrow, UI_FONT_TITLE, 0);
    lv_obj_set_style_text_color(arrow, UI_COLOR_ACCENT, 0);
    lv_obj_center(arrow);

    lv_obj_t *lbl = lv_label_create(bar);
    lv_label_set_text(lbl, title);
    lv_obj_set_style_text_font(lbl, UI_FONT_TITLE, 0);
    lv_obj_set_style_text_color(lbl, UI_COLOR_TEXT, 0);
    /* 58 = 返回键右边缘(4+46) 再留 8 的间距，按钮变宽后这里要跟着挪，
     * 否则标题会被压在按钮下面 */
    lv_obj_align(lbl, LV_ALIGN_LEFT_MID, 58, 0);

    /* 内容区 */
    lv_obj_t *content = lv_obj_create(root);
    lv_obj_set_width(content, LV_PCT(100));
    lv_obj_set_flex_grow(content, 1);
    lv_obj_set_style_bg_opa(content, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(content, 0, 0);
    lv_obj_set_style_radius(content, 0, 0);
    lv_obj_set_style_pad_all(content, UI_PAD, 0);
    lv_obj_set_style_pad_row(content, 6, 0);
    lv_obj_set_flex_flow(content, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_scrollbar_mode(content, LV_SCROLLBAR_MODE_AUTO);

    /* 内容区挂在 root 上供取用。注意返回的必须是 root：页面切换时
     * lv_obj_del() 删的是返回值，只返回 content 会把标题栏和返回按钮
     * 整条留在屏幕上（而且每进一次子页就多留一条）。 */
    lv_obj_set_user_data(root, content);
    return root;
}

lv_obj_t *ui_page_shell_content(lv_obj_t *shell)
{
    return (lv_obj_t *)lv_obj_get_user_data(shell);
}

lv_obj_t *ui_list_row_create(lv_obj_t *parent, const char *label,
                             const char *value, bool show_arrow)
{
    lv_obj_t *row = lv_obj_create(parent);
    lv_obj_set_size(row, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_style_bg_color(row, UI_COLOR_CARD, 0);
    lv_obj_set_style_bg_opa(row, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(row, 0, 0);
    lv_obj_set_style_radius(row, UI_RADIUS_BTN, 0);
    lv_obj_set_style_pad_all(row, 8, 0);
    lv_obj_set_style_pad_left(row, 12, 0);
    lv_obj_set_style_pad_right(row, 12, 0);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *name = lv_label_create(row);
    lv_label_set_text(name, label);
    lv_obj_set_style_text_font(name, UI_FONT_BODY, 0);
    lv_obj_set_style_text_color(name, UI_COLOR_TEXT, 0);
    lv_obj_align(name, LV_ALIGN_LEFT_MID, 0, 0);

    if (show_arrow) {
        lv_obj_t *arrow = lv_label_create(row);
        lv_label_set_text(arrow, LV_SYMBOL_RIGHT);
        lv_obj_set_style_text_font(arrow, UI_FONT_BODY, 0);
        lv_obj_set_style_text_color(arrow, UI_COLOR_TEXT_SUB, 0);
        lv_obj_align(arrow, LV_ALIGN_RIGHT_MID, 0, 0);
    }

    lv_obj_t *val = lv_label_create(row);
    lv_label_set_text(val, value ? value : "");
    lv_obj_set_style_text_font(val, UI_FONT_SMALL, 0);
    lv_obj_set_style_text_color(val, UI_COLOR_TEXT_SUB, 0);
    lv_obj_align(val, LV_ALIGN_RIGHT_MID, show_arrow ? -18 : 0, 0);
    /* 让 ui_list_row_set_value() 能找回这一行右侧的值文本 */
    lv_obj_set_user_data(row, val);

    return row;
}

void ui_list_row_set_value(lv_obj_t *row, const char *value)
{
    lv_obj_t *val = (lv_obj_t *)lv_obj_get_user_data(row);
    if (val != NULL) {
        lv_label_set_text(val, value ? value : "");
    }
}

lv_obj_t *ui_list_row_create_clickable(lv_obj_t *parent, const char *label,
                                       const char *value, bool show_arrow,
                                       lv_event_cb_t cb, void *user)
{
    lv_obj_t *row = ui_list_row_create(parent, label, value, show_arrow);
    lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
    if (cb != NULL) {
        lv_obj_add_event_cb(row, cb, LV_EVENT_CLICKED, user);
    }
    return row;
}

lv_obj_t *ui_section_create(lv_obj_t *parent, const char *title)
{
    lv_obj_t *lbl = lv_label_create(parent);
    lv_label_set_text(lbl, title);
    lv_obj_set_style_text_font(lbl, UI_FONT_SMALL, 0);
    lv_obj_set_style_text_color(lbl, UI_COLOR_TEXT_SUB, 0);
    /* 组间空隙用 pad_top 做（LVGL 8 没有 margin） */
    lv_obj_set_style_pad_top(lbl, 8, 0);
    return lbl;
}

lv_obj_t *ui_list_note_create(lv_obj_t *parent, const char *text)
{
    lv_obj_t *lbl = lv_label_create(parent);
    lv_label_set_text(lbl, text);
    lv_obj_set_style_text_font(lbl, UI_FONT_SMALL, 0);
    lv_obj_set_style_text_color(lbl, UI_COLOR_TEXT_SUB, 0);
    /* 跟在某一行后面，往上收一点、往左缩一点，读起来是"附在那行下面的" */
    lv_obj_set_style_pad_top(lbl, 2, 0);
    lv_obj_set_style_pad_left(lbl, 4, 0);
    /* 自动换行：说明都不短，不换行会顶出屏幕 */
    lv_label_set_long_mode(lbl, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(lbl, LV_PCT(100));
    return lbl;
}

lv_obj_t *ui_round_button_create(lv_obj_t *parent, int diameter, lv_color_t color)
{
    lv_obj_t *btn = lv_btn_create(parent);
    lv_obj_set_size(btn, diameter, diameter);
    lv_obj_set_style_radius(btn, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(btn, color, 0);
    lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, 0);
    lv_obj_set_style_shadow_width(btn, 0, 0);
    return btn;
}

/*---------------------------------------------------------------------------
 * 提示条
 *
 * label 只建一次，之后靠显示/隐藏复用——反复 create/del 在设置页这种
 * 会被来回进出的地方更容易出生命周期问题。
 *-------------------------------------------------------------------------*/
#define TOAST_HOLD_MS 1800

static lv_obj_t   *s_toast = NULL;
static lv_timer_t *s_toast_timer = NULL;

static void toast_timer_cb(lv_timer_t *t)
{
    (void)t;
    if (s_toast != NULL) {
        lv_obj_add_flag(s_toast, LV_OBJ_FLAG_HIDDEN);
    }
    if (s_toast_timer != NULL) {
        lv_timer_del(s_toast_timer);
        s_toast_timer = NULL;
    }
}

void ui_toast_show(const char *text, lv_color_t color)
{
    if (s_toast == NULL) {
        s_toast = lv_label_create(lv_layer_top());
        lv_obj_set_style_text_font(s_toast, UI_FONT_SMALL, 0);
        lv_obj_set_style_bg_color(s_toast, lv_color_black(), 0);
        lv_obj_set_style_bg_opa(s_toast, LV_OPA_70, 0);
        lv_obj_set_style_radius(s_toast, 6, 0);
        lv_obj_set_style_pad_all(s_toast, 5, 0);
        lv_obj_clear_flag(s_toast, LV_OBJ_FLAG_SCROLLABLE);
        /* 压在标题栏下面，不挡住返回键 */
        lv_obj_align(s_toast, LV_ALIGN_TOP_MID, 0, UI_TITLE_BAR_H + 8);
        lv_obj_add_flag(s_toast, LV_OBJ_FLAG_HIDDEN);
    }

    lv_label_set_text(s_toast, text ? text : "");
    lv_obj_set_style_text_color(s_toast, color, 0);
    lv_obj_clear_flag(s_toast, LV_OBJ_FLAG_HIDDEN);
    /* 必须抬到 lv_layer_top() 的最前面。提示条是懒创建的：如果它比
     * 某个覆盖层（相册大图、软键盘）先建好，后建的覆盖层就会把它压在
     * 下面——"Sending..." 和发送结果全都看不见，而且不报错。 */
    lv_obj_move_foreground(s_toast);

    /* 重新计时：连着弹两条时，第二条要拿满自己那 1.8 秒 */
    if (s_toast_timer != NULL) {
        lv_timer_del(s_toast_timer);
    }
    s_toast_timer = lv_timer_create(toast_timer_cb, TOAST_HOLD_MS, NULL);
}
