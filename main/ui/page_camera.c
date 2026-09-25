/*
 * page_camera.c — 取景页
 *
 * 摄像头画面铺满 320x240，零拷贝挂到 lv_img 上：每帧只把 lv_img_dsc_t 的
 * data 指针指向摄像头缓冲区，不做 memcpy。
 *
 * 覆盖层：顶部半透明条（返回 + 标题），底部半透明条（圆形快门）。
 * 底部/顶部的"半透明"在 320x240 上直接用半透明纯色，不做实时模糊。
 */
#include <stdlib.h>
#include <string.h>

#include "esp_log.h"
#include "esp_lvgl_port.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "app_audio.h"
#include "app_camera.h"
#include "app_sound.h"
#include "app_storage.h"
#include "lckfb_bsp.h"
#include "ui.h"
#include "ui_theme.h"

static const char *TAG = "page_camera";

/* 顶部条高一点、返回键大一点：取景页的返回键是全屏覆盖层上的悬浮按钮，
 * 太小的话既难点中也让人不确定点没点中 */
#define TOP_BAR_H     30
#define BACK_BTN_W    48
#define BOTTOM_BAR_H  56
#define SHUTTER_SIZE  40

static lv_obj_t      *s_img = NULL;
static lv_img_dsc_t   s_dsc;
static lv_obj_t      *s_shutter = NULL;
static lv_obj_t      *s_toast = NULL;
static lv_timer_t    *s_toast_timer = NULL;

/* 正在等 capture_task 回来。触摸快门靠 LV_STATE_DISABLED 挡连点，
 * Boot 键是轮询的、没有"禁用"状态可言，所以两边都得看这个标志 */
static bool           s_capturing = false;

/* Boot 键轮询状态 */
static lv_timer_t    *s_key_timer = NULL;
static uint8_t        s_key_press_count = 0;
static bool           s_key_armed = true;   /* false = 本轮已拍过，等松开 */

/* 拍完定格 */
static bool           s_freeze = false;
static lv_timer_t    *s_freeze_timer = NULL;

/*---------------------------------------------------------------------------
 * 取帧回调（在摄像头任务上下文里执行）
 *-------------------------------------------------------------------------*/
static void on_frame(const uint8_t *rgb565, uint32_t w, uint32_t h, void *user)
{
    (void)user;
    if (s_img == NULL) {
        return;
    }
    /* 定格期间把取帧丢掉，屏幕自然就停在刚才那一帧 */
    if (s_freeze) {
        return;
    }
    if (!lvgl_port_lock(50)) {
        return;                 /* 拿不到锁就丢这一帧，不要卡住摄像头任务 */
    }
    s_dsc.header.w = (lv_coord_t)w;
    s_dsc.header.h = (lv_coord_t)h;
    s_dsc.data_size = w * h * 2;
    s_dsc.data = rgb565;
    /* 每帧都清一次图片解码缓存：我们复用的是同一个 lv_img_dsc_t 指针，
     * 只换里面的 data。缓存按源指针命中，不清的话 LVGL 会一直拿第一帧
     * 缓存的 data 指针去画，画面就卡住不动了。
     * TRUE_COLOR 变量源本来就没有真正需要缓存的东西，清掉的代价可以忽略。 */
    lv_img_cache_invalidate_src(NULL);
    lv_img_set_src(s_img, &s_dsc);
    lvgl_port_unlock();
}

/*---------------------------------------------------------------------------
 * 提示条
 *-------------------------------------------------------------------------*/
static void hide_toast(void)
{
    if (s_toast != NULL) {
        lv_obj_add_flag(s_toast, LV_OBJ_FLAG_HIDDEN);
    }
}

static void toast_timer_cb(lv_timer_t *t)
{
    (void)t;
    hide_toast();
    if (s_toast_timer != NULL) {
        lv_timer_del(s_toast_timer);
        s_toast_timer = NULL;
    }
}

static void show_toast(const char *text, lv_color_t color)
{
    if (s_toast == NULL) {
        return;
    }
    lv_label_set_text(s_toast, text);
    lv_obj_set_style_text_color(s_toast, color, 0);
    lv_obj_clear_flag(s_toast, LV_OBJ_FLAG_HIDDEN);

    if (s_toast_timer != NULL) {
        lv_timer_del(s_toast_timer);
    }
    s_toast_timer = lv_timer_create(toast_timer_cb, 1800, NULL);
}

/*---------------------------------------------------------------------------
 * 拍照
 *-------------------------------------------------------------------------*/
static void capture_done_async(void *arg)
{
    /* 已回到 LVGL 上下文，可以安全操作控件 */
    char *path = (char *)arg;
    if (path != NULL) {
        /* 只显示文件名，完整路径太长放不下 */
        const char *name = strrchr(path, '/');
        show_toast(name ? name + 1 : path, lv_color_white());
        free(path);
    } else {
        show_toast("Save failed", UI_COLOR_DANGER);
    }
    s_capturing = false;
    if (s_shutter != NULL) {
        lv_obj_clear_state(s_shutter, LV_STATE_DISABLED);
    }
}

static void capture_task(void *arg)
{
    char path[96] = {0};
    esp_err_t err = app_camera_capture(path, sizeof(path));
    /* 编码 + 写卡耗时几百毫秒，不能占用 LVGL 任务，所以放在这里做，
     * 再用 lv_async_call 把结果送回 UI 线程。 */
    lv_async_call(capture_done_async, err == ESP_OK ? strdup(path) : NULL);
    vTaskDelete(NULL);
}

static void back_cb(lv_event_t *e)
{
    (void)e;
    ui_go_home_async();
}

/*---------------------------------------------------------------------------
 * 快门声
 *
 * 必须放自己的任务里：app_sound_play_pcm() 会阻塞在 I2S 写入上（写完还要等
 * DMA 排空），占着 LVGL 任务的话这段时间触摸采样整段丢失。
 *
 * 优先级 5 比 LVGL 的 4 高，是为了让"咔"紧跟快门，而不是等界面这一轮画完。
 * 它在 I2S 写入上很快就把 CPU 让出去了，不会真把界面压住。
 *-------------------------------------------------------------------------*/
static void shutter_sound_task(void *arg)
{
    (void)arg;
    app_sound_play_shutter();
    vTaskDelete(NULL);
}

static void play_shutter_sound(void)
{
    if (!app_sound_shutter_enabled()) {
        return;
    }
    /* 建任务失败就只是没声音，不弹提示：拍照本身成功了，用户看到的是照片。
     * app_sound 那边会把失败原因打进串口 */
    if (xTaskCreate(shutter_sound_task, "shutter_snd", 3072, NULL, 5, NULL) != pdPASS) {
        ESP_LOGW(TAG, "no memory for shutter sound task");
    }
}

/*---------------------------------------------------------------------------
 * 拍完定格
 *-------------------------------------------------------------------------*/
#define FREEZE_MS 1000

static void freeze_timer_cb(lv_timer_t *t)
{
    (void)t;
    s_freeze = false;
    if (s_freeze_timer != NULL) {
        lv_timer_del(s_freeze_timer);
        s_freeze_timer = NULL;
    }
}

/* 触摸快门和 Boot 键共用这一段，避免两份拍照逻辑各自演进 */
static void trigger_capture(void)
{
    if (s_capturing) {
        return;
    }
    if (!app_storage_ready()) {
        show_toast("No TF card", UI_COLOR_DANGER);
        return;
    }
    if (!app_camera_is_running()) {
        show_toast("Camera busy", UI_COLOR_DANGER);
        return;
    }

    s_capturing = true;
    /* 立刻禁用快门，避免连点重复触发 */
    if (s_shutter != NULL) {
        lv_obj_add_state(s_shutter, LV_STATE_DISABLED);
    }

    play_shutter_sound();

    /*
     * 定格 1 秒，让用户看清拍到的画面。
     *
     * 这里冻结的是预览的最后一帧，而不是去读刚存下来的那个 JPEG 文件：
     * 预览本来就是同一颗摄像头、同一分辨率，肉眼分不出差别，却省掉了
     * 读卡 + JPEG 解码 + 153KB PSRAM。画面"停住"的效果是一样的。
     */
    s_freeze = true;
    if (s_freeze_timer != NULL) {
        lv_timer_del(s_freeze_timer);
    }
    s_freeze_timer = lv_timer_create(freeze_timer_cb, FREEZE_MS, NULL);

    show_toast("Saving...", lv_color_white());

    if (xTaskCreate(capture_task, "capture", 4096, NULL, 5, NULL) != pdPASS) {
        s_capturing = false;
        if (s_shutter != NULL) {
            lv_obj_clear_state(s_shutter, LV_STATE_DISABLED);
        }
        show_toast("Out of memory", UI_COLOR_DANGER);
    }
}

static void shutter_cb(lv_event_t *e)
{
    (void)e;
    trigger_capture();
}

/*---------------------------------------------------------------------------
 * 滤镜：快门左右各一个
 *
 * 走传感器寄存器（app_camera_set_effect），所以**预览和存下来的照片一起变**。
 * 两个按钮是一组单选，再点一次当前那个 = 关掉（回到无滤镜）——多一个"都不选"
 * 的出口，比在 56px 高的控制条里再塞一个 None 按钮省地方。
 *-------------------------------------------------------------------------*/
#define FILTER_BTN_W   60
#define FILTER_BTN_H   32
#define FILTER_BTN_OFS 80          /* 相对快门中心的水平偏移 */

static lv_obj_t *s_btn_filter[2] = { NULL, NULL };
static const app_camera_effect_t s_filter_effect[2] = {
    APP_CAM_EFFECT_GRAYSCALE, APP_CAM_EFFECT_NEGATIVE,
};
static const char *const s_filter_text[2] = { "B&W", "Neg" };

static void refresh_filter_buttons(void)
{
    const app_camera_effect_t cur = app_camera_get_effect();
    for (int i = 0; i < 2; i++) {
        lv_obj_t *b = s_btn_filter[i];
        if (b == NULL) {
            continue;
        }
        const bool on = (cur == s_filter_effect[i]);
        /* 选中态是强调色实心；没选中是白色半透明，压在底部那条黑色半透明
         * 控制条上刚好能看见，又不至于抢快门的注意力 */
        lv_obj_set_style_bg_color(b, on ? UI_COLOR_ACCENT : lv_color_white(), 0);
        lv_obj_set_style_bg_opa(b, on ? LV_OPA_COVER : LV_OPA_20, 0);
    }
}

static void filter_cb(lv_event_t *e)
{
    const int idx = (int)(intptr_t)lv_event_get_user_data(e);
    const app_camera_effect_t cur = app_camera_get_effect();
    const app_camera_effect_t next =
        (cur == s_filter_effect[idx]) ? APP_CAM_EFFECT_NORMAL : s_filter_effect[idx];

    esp_err_t err = app_camera_set_effect(next);
    if (err != ESP_OK) {
        /* 传感器或驱动不认这个特效。界面照样切过去没有意义，提示一句就停 */
        show_toast("Filter not available", UI_COLOR_DANGER);
        return;
    }
    refresh_filter_buttons();
}

static lv_obj_t *create_filter_button(lv_obj_t *parent, int idx)
{
    lv_obj_t *btn = lv_btn_create(parent);
    lv_obj_set_size(btn, FILTER_BTN_W, FILTER_BTN_H);
    lv_obj_set_style_radius(btn, FILTER_BTN_H / 2, 0);
    lv_obj_set_style_shadow_width(btn, 0, 0);
    lv_obj_set_ext_click_area(btn, 8);
    lv_obj_add_event_cb(btn, filter_cb, LV_EVENT_CLICKED, (void *)(intptr_t)idx);

    lv_obj_t *lbl = lv_label_create(btn);
    lv_label_set_text(lbl, s_filter_text[idx]);
    lv_obj_set_style_text_font(lbl, UI_FONT_BODY, 0);
    lv_obj_set_style_text_color(lbl, lv_color_white(), 0);
    lv_obj_center(lbl);

    return btn;
}

/*---------------------------------------------------------------------------
 * Boot 键（GPIO0）
 *
 * 轮询而不是中断：需要去抖，而且"按住不放"必须只拍一张——状态机得记住
 * 这一轮已经触发过，松开才重新武装。这两件事放中断里做反而更绕、更容易
 * 出竞态；50ms 轮询的开销可以忽略。
 *
 * 只在取景页生效：定时器随页面建立和销毁。相机本来就只在取景页运行
 * （app_camera_capture 要求 is_running），所以这也是唯一能拍的地方。
 *-------------------------------------------------------------------------*/
#define KEY_POLL_MS           50
#define KEY_DEBOUNCE_SAMPLES  3         /* 3 × 50ms = 150ms 确认按下 */

static void boot_key_timer_cb(lv_timer_t *t)
{
    (void)t;

    if (!bsp_boot_button_pressed()) {
        s_key_press_count = 0;
        s_key_armed = true;             /* 松开才允许下一次 */
        return;
    }
    if (!s_key_armed) {
        return;                         /* 还按着，且这一轮已经拍过 */
    }
    if (++s_key_press_count < KEY_DEBOUNCE_SAMPLES) {
        return;                         /* 还在去抖窗口内 */
    }
    s_key_press_count = 0;
    s_key_armed = false;
    trigger_capture();
}

/*---------------------------------------------------------------------------
 * 页面
 *-------------------------------------------------------------------------*/
lv_obj_t *page_camera_create(lv_obj_t *parent)
{
    lv_obj_t *root = lv_obj_create(parent);
    lv_obj_set_size(root, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_color(root, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(root, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(root, 0, 0);
    lv_obj_set_style_radius(root, 0, 0);
    lv_obj_set_style_pad_all(root, 0, 0);
    lv_obj_clear_flag(root, LV_OBJ_FLAG_SCROLLABLE);

    /* 取景画面：铺满整屏 */
    memset(&s_dsc, 0, sizeof(s_dsc));
    s_dsc.header.cf = LV_IMG_CF_TRUE_COLOR;
    s_dsc.header.always_zero = 0;
    s_dsc.header.w = BSP_LCD_H_RES;     /* 320，相机 QVGA 与此一致 */
    s_dsc.header.h = BSP_LCD_V_RES;     /* 240 */
    s_dsc.data_size = 0;

    s_img = lv_img_create(root);
    lv_obj_align(s_img, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_bg_color(s_img, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(s_img, LV_OPA_COVER, 0);

    /* 顶部条 */
    lv_obj_t *top = lv_obj_create(root);
    lv_obj_set_size(top, LV_PCT(100), TOP_BAR_H);
    lv_obj_align(top, LV_ALIGN_TOP_MID, 0, 0);
    lv_obj_set_style_bg_color(top, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(top, LV_OPA_50, 0);
    lv_obj_set_style_border_width(top, 0, 0);
    lv_obj_set_style_radius(top, 0, 0);
    lv_obj_set_style_pad_all(top, 0, 0);
    lv_obj_clear_flag(top, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *back = lv_btn_create(top);
    lv_obj_set_size(back, BACK_BTN_W, TOP_BAR_H - 4);
    lv_obj_align(back, LV_ALIGN_LEFT_MID, 4, 0);
    lv_obj_set_style_bg_opa(back, LV_OPA_TRANSP, 0);
    lv_obj_set_style_shadow_width(back, 0, 0);
    lv_obj_set_ext_click_area(back, 12);    /* 见 ui_theme.c 的说明 */

    lv_obj_add_event_cb(back, back_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *arrow = lv_label_create(back);
    lv_label_set_text(arrow, LV_SYMBOL_LEFT);
    lv_obj_set_style_text_font(arrow, UI_FONT_TITLE, 0);
    lv_obj_set_style_text_color(arrow, lv_color_white(), 0);
    lv_obj_center(arrow);

    /* 提示条：居中偏上，拍照结果在这里反馈 */
    s_toast = lv_label_create(root);
    lv_label_set_text(s_toast, "");
    lv_obj_set_style_text_font(s_toast, UI_FONT_SMALL, 0);
    lv_obj_set_style_text_color(s_toast, lv_color_white(), 0);
    lv_obj_set_style_bg_color(s_toast, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(s_toast, LV_OPA_70, 0);
    lv_obj_set_style_radius(s_toast, 6, 0);
    lv_obj_set_style_pad_all(s_toast, 5, 0);
    lv_obj_align(s_toast, LV_ALIGN_TOP_MID, 0, TOP_BAR_H + 8);
    lv_obj_add_flag(s_toast, LV_OBJ_FLAG_HIDDEN);

    /* 底部控制条 + 快门 */
    lv_obj_t *bottom = lv_obj_create(root);
    lv_obj_set_size(bottom, LV_PCT(100), BOTTOM_BAR_H);
    lv_obj_align(bottom, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_style_bg_color(bottom, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(bottom, LV_OPA_50, 0);
    lv_obj_set_style_border_width(bottom, 0, 0);
    lv_obj_set_style_radius(bottom, 0, 0);
    lv_obj_set_style_pad_all(bottom, 0, 0);
    lv_obj_clear_flag(bottom, LV_OBJ_FLAG_SCROLLABLE);

    s_shutter = ui_round_button_create(bottom, SHUTTER_SIZE, lv_color_white());
    lv_obj_center(s_shutter);
    lv_obj_set_style_outline_width(s_shutter, 3, 0);
    lv_obj_set_style_outline_color(s_shutter, UI_COLOR_TEXT_SUB, 0);
    lv_obj_set_style_outline_opa(s_shutter, LV_OPA_60, 0);
    lv_obj_set_style_outline_pad(s_shutter, 3, 0);
    lv_obj_add_event_cb(s_shutter, shutter_cb, LV_EVENT_CLICKED, NULL);

    /* 快门左右各一个滤镜。控制条 320 宽、快门 40 居中，两边各剩 140，
     * 放 60 宽的药丸按钮绰绰有余，偏移 80 之后离屏幕边缘还有 50 */
    s_btn_filter[0] = create_filter_button(bottom, 0);
    lv_obj_align(s_btn_filter[0], LV_ALIGN_CENTER, -FILTER_BTN_OFS, 0);
    s_btn_filter[1] = create_filter_button(bottom, 1);
    lv_obj_align(s_btn_filter[1], LV_ALIGN_CENTER, FILTER_BTN_OFS, 0);
    /* 恢复上一次的选择：滤镜存在 app_camera 里跨页面活着 */
    refresh_filter_buttons();

    /* Boot 键拍照：轮询定时器只在取景页存活 */
    s_capturing = false;
    if (bsp_boot_button_init() == ESP_OK) {
        s_key_press_count = 0;
        s_key_armed = true;
        s_key_timer = lv_timer_create(boot_key_timer_cb, KEY_POLL_MS, NULL);
        if (s_key_timer == NULL) {
            ESP_LOGW(TAG, "boot key timer create failed, touch shutter only");
        }
    } else {
        ESP_LOGW(TAG, "boot button unavailable, touch shutter only");
    }

    /*
     * 相机的入口就是音乐出口的守卫。
     *
     * 音乐现在是**模式**不是页面：离开音乐页不再停播（那是为了频谱灯，
     * 见 page_music.c），所以"相机和 MP3 不能同时跑"这条规矩的落点挪到了
     * 这里——两者抢内部 DMA RAM，是真会出问题的，不是风格问题。
     */
    if (app_audio_state() != APP_AUDIO_IDLE) {
        ESP_LOGW(TAG, "音乐还在放，先停掉（相机与 MP3 不能同时工作）");
        app_audio_stop();
    }

    /* 启动预览 */
    esp_err_t err = app_camera_start();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "camera start failed: %s", esp_err_to_name(err));
        show_toast("Camera unavailable", UI_COLOR_DANGER);
    } else {
        app_camera_set_frame_cb(on_frame, NULL);
    }

    return root;
}

void page_camera_destroy(void)
{
    /* 必须先注销回调：回调会去操作 s_img，而它马上就要被销毁了 */
    app_camera_set_frame_cb(NULL, NULL);
    app_camera_stop();

    if (s_key_timer != NULL) {
        lv_timer_del(s_key_timer);
        s_key_timer = NULL;
    }
    if (s_freeze_timer != NULL) {
        lv_timer_del(s_freeze_timer);
        s_freeze_timer = NULL;
    }
    s_freeze = false;           /* 下次进取景页从活动画面开始 */
    if (s_toast_timer != NULL) {
        lv_timer_del(s_toast_timer);
        s_toast_timer = NULL;
    }
    /* 拍照任务还可能在跑，它回调回来会看这些指针，全部置空 */
    s_img = NULL;
    s_shutter = NULL;
    s_toast = NULL;
    s_btn_filter[0] = NULL;
    s_btn_filter[1] = NULL;
}
