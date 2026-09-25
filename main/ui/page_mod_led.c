/*
 * page_mod_led.c — 模块控制页：WS2812 氛围灯
 *
 * 两个效果互斥，顶上两个按钮切：
 *
 *   Manual    色轮选色 + 亮度滑条，整块板一个颜色
 *   Spectrum  8 根柱子跟着音乐跳，信号来自解码后的 PCM（见 app_led2812.h）
 *
 * **离开这一页不会关灯**：设备和副屏一样跟的是"模式"不是"页面"。
 * 关掉要走右上角那个 Disable（它调的才是 app_module_deselect()）。
 *
 * 界面**永远不碰硬件**：所有动作都是往 app_led2812 里写一个值，渲染和发送
 * 在那个服务自己的任务里；频谱柱子读的是 100ms 一次的快照。
 */
#include "page_mod_led.h"

#include "esp_log.h"

#include "app_audio.h"
#include "app_led2812.h"
#include "app_module.h"
#include "ui.h"
#include "ui_theme.h"

static const char *TAG = "page_mod_led";

/*---------------------------------------------------------------------------
 * 排版（内容区是 304x190，每个数都是照着它算的）
 *-------------------------------------------------------------------------*/
/*
 * 排版。内容区 304x190，纵向是这么分掉的（和工作模式页一样的"卡片 + 动作行"）：
 *
 *   效果切换 30 + 间距 6 + 控制卡片 PANEL_H + 间距 6 + 关闭行 34 = 190
 *   → PANEL_H = 114，卡片内边距 8，可用 98
 *       manual   色轮 94 / 亮度条 88
 *       spectrum 8 根柱子（每根 28 宽，SPACE_BETWEEN 自动摊开）+ 播放行 28
 */
#define BTN_H       30
#define CW_SIZE     94      /* 色轮 */
#define SLIDER_W    20
#define SLIDER_H    88
#define PANEL_H     114
#define BAR_W       28
#define BAR_H       64
#define BAR_NUM     APP_LED_COLS

static lv_obj_t   *s_btn_manual;
static lv_obj_t   *s_btn_spectrum;
static lv_obj_t   *s_panel_manual;
static lv_obj_t   *s_panel_spectrum;
static lv_obj_t   *s_wheel;
static lv_obj_t   *s_bright;
static lv_obj_t   *s_bright_label;
static lv_obj_t   *s_bars[BAR_NUM];
static lv_obj_t   *s_track_label;
static lv_obj_t   *s_play_label;
static lv_timer_t *s_timer;

/* 一张白卡片：两套控制面板都装在这里面，视觉上和工作模式页的列表一致 */
static lv_obj_t *panel_card(lv_obj_t *parent)
{
    lv_obj_t *card = ui_card_create(parent);
    lv_obj_set_size(card, LV_PCT(100), PANEL_H);
    lv_obj_set_style_pad_all(card, 8, 0);
    lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);
    return card;
}

/*---------------------------------------------------------------------------
 * 两个效果的按钮状态
 *-------------------------------------------------------------------------*/
static void effect_buttons_refresh(void)
{
    const bool spec = (app_led2812_effect() == APP_LED_EFFECT_SPECTRUM);
    lv_obj_t *on  = spec ? s_btn_spectrum : s_btn_manual;
    lv_obj_t *off = spec ? s_btn_manual : s_btn_spectrum;

    /*
     * 选中 = 强调色实心；没选中 = **和卡片一样的白底**（不是透明）。
     * 透明的话没选中的那个会直接坐在页面灰底上，看着像"不可用"；
     * 白底才读得出"它也是个能点的卡片，只是现在没选它"。
     */
    lv_obj_set_style_bg_color(on, UI_COLOR_ACCENT, 0);
    lv_obj_set_style_bg_opa(on, LV_OPA_COVER, 0);
    lv_obj_set_style_text_color(lv_obj_get_child(on, 0), lv_color_white(), 0);

    lv_obj_set_style_bg_color(off, UI_COLOR_CARD, 0);
    lv_obj_set_style_bg_opa(off, LV_OPA_COVER, 0);
    lv_obj_set_style_text_color(lv_obj_get_child(off, 0), UI_COLOR_TEXT, 0);

    if (spec) {
        lv_obj_clear_flag(s_panel_spectrum, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(s_panel_manual, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_clear_flag(s_panel_manual, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(s_panel_spectrum, LV_OBJ_FLAG_HIDDEN);
    }
}

static void effect_cb(lv_event_t *e)
{
    const app_led_effect_t which =
        (app_led_effect_t)(intptr_t)lv_event_get_user_data(e);
    const esp_err_t err = app_led2812_set_effect(which);
    if (err != ESP_OK) {
        ui_toast_show(esp_err_to_name(err), UI_COLOR_DANGER);
        return;
    }
    effect_buttons_refresh();
}

/*---------------------------------------------------------------------------
 * 手动氛围灯
 *-------------------------------------------------------------------------*/
static void wheel_cb(lv_event_t *e)
{
    (void)e;
    /* 色轮给的是 0~360 / 0~100，服务那边是 0~255 的两路，换算一下 */
    const lv_color_hsv_t hsv = lv_colorwheel_get_hsv(s_wheel);
    app_led2812_set_hsv((uint8_t)((uint32_t)hsv.h * 255 / 360),
                        (uint8_t)((uint32_t)hsv.s * 255 / 100));
}

static void bright_cb(lv_event_t *e)
{
    lv_obj_t *sl = lv_event_get_target(e);
    /* 拖动时每格都发：这个值在服务里就是一次赋值，帧是任务自己按 30fps 发的，
     * 不会变成"拖一下发一帧灯" */
    const uint8_t v = (uint8_t)lv_slider_get_value(sl);
    app_led2812_set_brightness(v);
    if (s_bright_label != NULL) {
        lv_label_set_text_fmt(s_bright_label, "%d%%", v);
    }
}

/*---------------------------------------------------------------------------
 * 频谱
 *-------------------------------------------------------------------------*/
static void player_refresh(void)
{
    if (s_track_label == NULL) {
        return;
    }
    const int idx = app_audio_current_index();
    const char *name = (idx >= 0) ? app_audio_track_name(idx) : NULL;
    if (name == NULL) {
        name = "No music";
    }
    /* 歌名可能是中文，必须显式换到带汉字的字体（见 ui_theme.h） */
    lv_obj_set_style_text_font(s_track_label, UI_FONT_CJK, 0);
    lv_label_set_text(s_track_label, name);

    const bool playing = (app_audio_state() == APP_AUDIO_PLAYING);
    lv_label_set_text(s_play_label, playing ? LV_SYMBOL_PAUSE : LV_SYMBOL_PLAY);
}

static void play_cb(lv_event_t *e)
{
    (void)e;
    /* 频谱灯要听到声音才动，而"放什么"是音乐页的事。这里只负责让它响：
     * 还没扫过列表就先扫一次（用户可能开机后直接来了这一页） */
    if (app_audio_init() != ESP_OK) {
        ui_toast_show("Audio init failed", UI_COLOR_DANGER);
        return;
    }
    if (app_audio_track_count() == 0) {
        if (app_audio_scan(NULL) <= 0) {
            ui_toast_show("No music on the TF card", UI_COLOR_DANGER);
            return;
        }
    }
    if (app_audio_state() == APP_AUDIO_IDLE) {
        app_audio_play_index(app_audio_current_index() < 0 ? 0
                                                          : app_audio_current_index());
    } else {
        app_audio_toggle();
    }
    player_refresh();
}

static void next_cb(lv_event_t *e)
{
    (void)e;
    app_audio_play_next();
    player_refresh();
}

/* 100ms 一拍：读柱子快照 + 刷新播放条。跑在 LVGL 任务里，所以能直接碰控件 */
static void ui_timer_cb(lv_timer_t *t)
{
    (void)t;
    uint8_t levels[BAR_NUM] = { 0 };
    app_led2812_get_levels(levels, BAR_NUM);
    for (int i = 0; i < BAR_NUM; i++) {
        lv_bar_set_value(s_bars[i], levels[i], LV_ANIM_OFF);
    }
    player_refresh();
}

/*---------------------------------------------------------------------------
 * 关模块
 *-------------------------------------------------------------------------*/
static void disable_cb(lv_event_t *e)
{
    (void)e;
    /* 停的是模块本身（不是这个页面）。停完回 Modules，那一页的 Running 会
     * 自己变回空的 */
    app_module_deselect();
    ui_open_modules();
}

/*---------------------------------------------------------------------------
 * 页面
 *-------------------------------------------------------------------------*/
static lv_obj_t *make_button(lv_obj_t *parent, const char *text, int w,
                             lv_event_cb_t cb, void *user)
{
    lv_obj_t *btn = lv_btn_create(parent);
    lv_obj_set_size(btn, w, BTN_H);
    lv_obj_set_style_radius(btn, UI_RADIUS_BTN, 0);
    lv_obj_set_style_shadow_width(btn, 0, 0);
    lv_obj_t *lbl = lv_label_create(btn);
    lv_label_set_text(lbl, text);
    lv_obj_set_style_text_font(lbl, UI_FONT_BODY, 0);
    lv_obj_center(lbl);
    if (cb != NULL) {
        lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, user);
    }
    return btn;
}

/* 返回键回 Modules 而不是主菜单：从哪进来的回哪去（用 _ex 那版外壳） */
static void back_cb(lv_event_t *e)
{
    (void)e;
    ui_open_modules();
}

lv_obj_t *page_mod_led_create(lv_obj_t *parent)
{
    lv_obj_t *shell = ui_page_shell_create_ex(parent, "Ambient Light", back_cb);
    lv_obj_t *content = ui_page_shell_content(shell);

    /* 顶上一条：两个效果按钮 + 右边一个 Disable */
    lv_obj_t *bar = lv_obj_create(content);
    lv_obj_set_size(bar, LV_PCT(100), BTN_H);
    lv_obj_set_style_bg_opa(bar, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(bar, 0, 0);
    lv_obj_set_style_pad_all(bar, 0, 0);
    lv_obj_set_flex_flow(bar, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(bar, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(bar, 6, 0);

    /* 两个效果按钮等宽铺满：选中=强调色，没选中=白卡片（见 effect_buttons_refresh） */
    s_btn_manual   = make_button(bar, "Manual", 148, effect_cb,
                                 (void *)(intptr_t)APP_LED_EFFECT_MANUAL);
    s_btn_spectrum = make_button(bar, "Spectrum", 148, effect_cb,
                                 (void *)(intptr_t)APP_LED_EFFECT_SPECTRUM);

    /* ---- 手动面板：一张白卡片里放色轮 + 竖排亮度 ---- */
    s_panel_manual = panel_card(content);

    s_wheel = lv_colorwheel_create(s_panel_manual, true);
    lv_obj_set_size(s_wheel, CW_SIZE, CW_SIZE);
    /* 只调色相：饱和度和明度固定在中间，亮度另有滑条。
     * mode_fixed 是必须的——不锁的话点一下色轮就换成调饱和度了 */
    lv_colorwheel_set_mode(s_wheel, LV_COLORWHEEL_MODE_HUE);
    lv_colorwheel_set_mode_fixed(s_wheel, true);
    {
        lv_color_hsv_t hsv = lv_colorwheel_get_hsv(s_wheel);
        hsv.h = 160;                    /* 和 app_led2812 的默认色对上 */
        lv_colorwheel_set_hsv(s_wheel, hsv);
    }
    lv_obj_align(s_wheel, LV_ALIGN_LEFT_MID, 0, 0);
    lv_obj_add_event_cb(s_wheel, wheel_cb, LV_EVENT_VALUE_CHANGED, NULL);

    s_bright = lv_slider_create(s_panel_manual);
    lv_obj_set_size(s_bright, SLIDER_W, SLIDER_H);
    lv_slider_set_range(s_bright, 0, 100);
    lv_slider_set_value(s_bright, app_led2812_brightness(), LV_ANIM_OFF);
    lv_obj_align(s_bright, LV_ALIGN_RIGHT_MID, -40, 0);
    lv_obj_add_event_cb(s_bright, bright_cb, LV_EVENT_VALUE_CHANGED, NULL);

    /* 滑条旁边直接写数值。原来只写了个 "Brightness" 标签，用户得自己
     * 从滑条位置估现在是几——音量那条滑条也是这么改的 */
    s_bright_label = lv_label_create(s_panel_manual);
    lv_obj_set_style_text_font(s_bright_label, UI_FONT_SMALL, 0);
    lv_obj_set_style_text_color(s_bright_label, UI_COLOR_TEXT_SUB, 0);
    lv_label_set_text_fmt(s_bright_label, "%d%%", app_led2812_brightness());
    lv_obj_align(s_bright_label, LV_ALIGN_RIGHT_MID, 0, 0);

    /* ---- 频谱面板：8 根柱子 + 一行播放控制 ---- */
    s_panel_spectrum = panel_card(content);

    lv_obj_t *bars = lv_obj_create(s_panel_spectrum);
    lv_obj_set_size(bars, LV_PCT(100), BAR_H);
    lv_obj_set_style_bg_opa(bars, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(bars, 0, 0);
    lv_obj_set_style_pad_all(bars, 0, 0);
    lv_obj_set_flex_flow(bars, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(bars, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_END,
                          LV_FLEX_ALIGN_END);
    lv_obj_align(bars, LV_ALIGN_TOP_MID, 0, 0);

    for (int i = 0; i < BAR_NUM; i++) {
        s_bars[i] = lv_bar_create(bars);
        lv_obj_set_size(s_bars[i], BAR_W, BAR_H);
        /* 柱子的量纲就是"亮几行"，0~APP_LED_ROWS，和服务那边一致 */
        lv_bar_set_range(s_bars[i], 0, APP_LED_ROWS);
        lv_bar_set_value(s_bars[i], 0, LV_ANIM_OFF);
        lv_obj_set_style_radius(s_bars[i], 3, LV_PART_MAIN);
        lv_obj_set_style_radius(s_bars[i], 3, LV_PART_INDICATOR);
        /* 低频在左、高频在右，和灯板上那一列一致（不然对着灯看会反） */
        lv_obj_set_style_bg_color(s_bars[i], UI_COLOR_ACCENT, LV_PART_INDICATOR);
    }

    /* 播放控制：频谱灯要有声音才动，而换歌是音乐页的事，这里只给
     * "让它响"和"换一首"两个动作，省得用户为了开声音跑来跑去 */
    lv_obj_t *play_row = lv_obj_create(s_panel_spectrum);
    lv_obj_set_size(play_row, LV_PCT(100), 28);
    lv_obj_align(play_row, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_style_bg_opa(play_row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(play_row, 0, 0);
    lv_obj_set_style_pad_all(play_row, 0, 0);
    lv_obj_clear_flag(play_row, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *btn_play = make_button(play_row, LV_SYMBOL_PLAY, 34, play_cb, NULL);
    lv_obj_align(btn_play, LV_ALIGN_LEFT_MID, 0, 0);
    s_play_label = lv_obj_get_child(btn_play, 0);

    lv_obj_t *btn_next = make_button(play_row, LV_SYMBOL_NEXT, 34, next_cb, NULL);
    lv_obj_align(btn_next, LV_ALIGN_LEFT_MID, 40, 0);

    s_track_label = lv_label_create(play_row);
    lv_label_set_long_mode(s_track_label, LV_LABEL_LONG_DOT);
    lv_obj_set_width(s_track_label, 180);
    lv_obj_set_style_text_color(s_track_label, UI_COLOR_TEXT, 0);
    lv_obj_align(s_track_label, LV_ALIGN_RIGHT_MID, 0, 0);

    /*
     * 底部一整行是"关模块"，和工作模式页那几行一个样子（白卡片 + 深色字）。
     * **标签用 "Turn off light" 而不是 "Disable"**：后者含糊到用户要问
     * "这是什么功能"。这行说的是灯，不是页面——返回键只是走人。
     */
    lv_obj_t *off_row = ui_list_row_create_clickable(content, "Turn off light", "",
                                                     false, disable_cb, NULL);
    /* 把那一行的标签染成危险色。ui_list_row_create 没留这个口子，
     * 取它的第一个子对象（就是标签）来染 */
    if (off_row != NULL && lv_obj_get_child_cnt(off_row) > 0) {
        lv_obj_set_style_text_color(lv_obj_get_child(off_row, 0), UI_COLOR_DANGER, 0);
    }

    effect_buttons_refresh();
    player_refresh();

    /* 100ms 一拍。**只读快照**，不触发任何硬件动作 */
    s_timer = lv_timer_create(ui_timer_cb, 100, NULL);

    if (!app_led2812_is_active()) {
        /* 正常进不来（Modules 页先 select 再跳）。万一路径变了走到这里，
         * 说清楚比让人对着不动的界面猜好 */
        ui_toast_show("Module not running", UI_COLOR_DANGER);
    }

    ESP_LOGI(TAG, "led page created (effect=%d, active=%d)",
             (int)app_led2812_effect(), (int)app_led2812_is_active());
    return shell;
}

void page_mod_led_destroy(void)
{
    if (s_timer != NULL) {
        lv_timer_del(s_timer);
        s_timer = NULL;
    }
    /* **不停模块**：灯跟的是"模式"不是"页面"。要关得走页面上那个 Disable，
     * 那才是 app_module_deselect() */
    s_btn_manual = s_btn_spectrum = NULL;
    s_panel_manual = s_panel_spectrum = NULL;
    s_wheel = s_bright = s_bright_label = NULL;
    s_track_label = s_play_label = NULL;
    for (int i = 0; i < BAR_NUM; i++) {
        s_bars[i] = NULL;
    }
}
