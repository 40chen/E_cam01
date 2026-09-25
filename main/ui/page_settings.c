/*
 * page_settings.c — 设置页
 *
 * 分组列表：Network（WiFi 凭据 + 实时状态）/ Mail（SMTP 与邮箱）/ Storage
 * （只读剩余空间）/ Sound（快门声开关 + 音量）/ Reset（带二次确认的擦除）。
 * 凭据通过 ui_input 软键盘输入，存 NVS（app_settings），保存 SSID 后立即
 * 发起连接（app_wifi）。
 *
 * 会改变整机形态的开关（U 盘模式、副屏）**不在这里**，在工作模式页。
 *
 * 状态行用 lv_timer 每秒刷新，和 page_home.c 刷 TF 卡图标同一套路。
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "app_audio.h"
#include "app_imu.h"
#include "app_mail.h"
#include "app_settings.h"
#include "app_sound.h"
#include "app_storage.h"
#include "app_wifi.h"
#include "lckfb_bsp.h"
#include "lvgl.h"
#include "ui.h"
#include "ui_input.h"
#include "ui_scan.h"
#include "ui_theme.h"

static const char *TAG = "page_settings";

static lv_obj_t *s_row_ssid = NULL;
static lv_obj_t *s_row_pass = NULL;
static lv_obj_t *s_row_status = NULL;
static lv_obj_t *s_row_host = NULL;
static lv_obj_t *s_row_port = NULL;
static lv_obj_t *s_row_from = NULL;
static lv_obj_t *s_row_mailpass = NULL;
static lv_obj_t *s_row_to = NULL;
static lv_obj_t *s_row_free = NULL;
static lv_obj_t *s_row_testmail = NULL;
static lv_timer_t *s_status_timer = NULL;

/*---------------------------------------------------------------------------
 * 小工具
 *
 * 两个都只是 ui_theme 里公共控件的薄包装：这里的调用点有二十来处，全部改成
 * 直接调公共版本会把这一页的 diff 摊得很大，而包装的意义就在于"设置页的列表
 * 长什么样"这个约定只写一处。工作模式页用的是同一对公共控件，两边排版一致。
 *-------------------------------------------------------------------------*/
static lv_obj_t *add_section(lv_obj_t *parent, const char *title)
{
    return ui_section_create(parent, title);
}

static lv_obj_t *add_row(lv_obj_t *parent, const char *label, const char *value,
                         bool arrow, lv_event_cb_t cb, void *user)
{
    return ui_list_row_create_clickable(parent, label, value, arrow, cb, user);
}

/*
 * 带开关的列表行。返回开关对象（挂回调用）。
 *
 * 两件事 ui_list_row_create 不管，这里补上：
 *   1. **值文本要让开右边的开关**：它默认右对齐在 -0，而开关占最右 44px，
 *      直接写值会糊在开关底下。挪到 -50。（现在没有开关行写值文本，但这条
 *      约束是真的踩出来过的——哪天真要往这儿写字，别再从 -0 开始试。）
 *   2. 开关的 user_data 指向**行**，回调里要拿行（比如写值文本）就从这儿取。
 */
static lv_obj_t *add_switch_row(lv_obj_t *parent, const char *label,
                                lv_event_cb_t cb)
{
    lv_obj_t *row = ui_list_row_create(parent, label, "", false);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_CLICKABLE);      /* 只有开关可点 */

    lv_obj_t *val = (lv_obj_t *)lv_obj_get_user_data(row);
    if (val != NULL) {
        lv_obj_align(val, LV_ALIGN_RIGHT_MID, -50, 0);
    }

    lv_obj_t *sw = lv_switch_create(row);
    lv_obj_set_size(sw, 44, 22);
    lv_obj_align(sw, LV_ALIGN_RIGHT_MID, 0, 0);
    lv_obj_set_user_data(sw, row);
    lv_obj_add_event_cb(sw, cb, LV_EVENT_VALUE_CHANGED, NULL);
    return sw;
}

/*---------------------------------------------------------------------------
 * WiFi：扫描选网
 *-------------------------------------------------------------------------*/
static void wifi_pass_done(const char *text, void *user)
{
    if (text != NULL) {
        app_settings_set_wifi_pass(text);
    }
    ui_list_row_set_value(s_row_pass,
                          app_settings_wifi_pass()[0] ? "****" : "Not set");
    if (app_settings_has_wifi()) {
        app_wifi_connect(app_settings_wifi_ssid(), app_settings_wifi_pass());
    }
    (void)user;
}

/*
 * 扫到网络、用户输完密码之后。user 是从 scan_picked 带下来的 SSID，
 * 堆上分配的，这个回调负责 free。
 */
static void picked_pass_done(const char *text, void *user)
{
    char *ssid = (char *)user;

    if (text == NULL) {
        /* 取消：什么都不存。注意别在这里先把 SSID 存下来，否则用户一取消
         * 就留下一个没有密码的网络，下次开机会拿空密码去连、必然认证失败 */
        free(ssid);
        return;
    }

    app_settings_set_wifi_ssid(ssid);
    app_settings_set_wifi_pass(text);
    ui_list_row_set_value(s_row_ssid, app_settings_wifi_ssid());
    ui_list_row_set_value(s_row_pass,
                          app_settings_wifi_pass()[0] ? "****" : "Not set");
    app_wifi_connect(app_settings_wifi_ssid(), app_settings_wifi_pass());
    free(ssid);
}

/* ui_scan 的选中回调。已在 LVGL 上下文（ui_scan 内部 lv_async_call 过） */
static void scan_picked(const char *ssid, bool needs_pass, void *user)
{
    (void)user;
    if (ssid == NULL || ssid[0] == '\0') {
        return;
    }

    if (!needs_pass) {
        /* 开放网络，没有密码这一步 */
        app_settings_set_wifi_ssid(ssid);
        app_settings_set_wifi_pass("");
        ui_list_row_set_value(s_row_ssid, app_settings_wifi_ssid());
        ui_list_row_set_value(s_row_pass, "Not set");
        app_wifi_connect(app_settings_wifi_ssid(), "");
        ui_toast_show("Connecting...", lv_color_white());
        return;
    }

    /* SSID 先攒在手里，等密码确认了再和密码一起落盘。
     * 这里什么都别提前写：用户要是取消了，原来那套凭据得原封不动，
     * 否则会把一个本来能连的网络弄成"有名字没密码"。 */
    char *held = strdup(ssid);
    if (held == NULL) {
        ui_toast_show("Out of memory", UI_COLOR_DANGER);
        return;
    }
    ui_input_open("Password", "", true, false, APP_SETTINGS_PASS_MAX,
                  picked_pass_done, held);
}

/*---------------------------------------------------------------------------
 * Mail 输入回调
 *-------------------------------------------------------------------------*/
static void host_done(const char *text, void *user)
{
    if (text != NULL) {
        app_settings_set_smtp_host(text);
    }
    ui_list_row_set_value(s_row_host, app_settings_smtp_host());
    (void)user;
}

static void port_done(const char *text, void *user)
{
    if (text != NULL) {
        app_settings_set_smtp_port(text);
    }
    ui_list_row_set_value(s_row_port, app_settings_smtp_port());
    (void)user;
}

/*
 * 上一次自动填进去的 SMTP 服务器。用来区分"这个值是我填的"和"这是用户
 * 自己敲的"——后者绝不能被覆盖。
 */
static char s_autofilled_host[APP_SETTINGS_HOST_MAX + 1];

/*
 * 按发件人域名自动补 SMTP 服务器（并顺手把端口补成 465）。
 *
 * 只在两种情况下覆盖 SMTP Server：还空着，或者当前值就是上次自动填的
 * （用户换了邮箱域名，服务器得跟着变）。用户手填过的一律不动——企业邮箱
 * 和自建服务器认域名认不出来，覆盖掉等于把他填的删了。
 */
static void autofill_smtp(void)
{
    const app_mail_smtp_hint_t *h = app_mail_guess_smtp(app_settings_mail_from());
    if (h == NULL) {
        return;                 /* 不认识的域名：保持原样，让用户自己填 */
    }
    if (!h->usable) {
        /* 认出来了但本机发不了（如微软系要 OAuth2）。这里**不能**填一个
         * 连不上的地址，否则用户只会看到一个没头没尾的连接失败 */
        ui_toast_show(h->reason, UI_COLOR_DANGER);
        return;
    }

    const char *cur = app_settings_smtp_host();
    if (cur[0] != '\0' && strcmp(cur, s_autofilled_host) != 0) {
        return;                 /* 用户手填过，别动 */
    }

    app_settings_set_smtp_host(h->host);
    strlcpy(s_autofilled_host, h->host, sizeof(s_autofilled_host));
    if (s_row_host != NULL) {
        ui_list_row_set_value(s_row_host, app_settings_smtp_host());
    }

    if (app_settings_smtp_port()[0] == '\0') {
        app_settings_set_smtp_port("465");
        if (s_row_port != NULL) {
            ui_list_row_set_value(s_row_port, app_settings_smtp_port());
        }
    }

    /* 下面那行刚被自动改了值，说一声，让用户知道是自己填上来的 */
    char msg[APP_SETTINGS_HOST_MAX + 8];
    snprintf(msg, sizeof(msg), "SMTP: %s", h->host);
    ui_toast_show(msg, lv_color_white());
}

static void from_done(const char *text, void *user)
{
    if (text != NULL) {
        app_settings_set_mail_from(text);
    }
    ui_list_row_set_value(s_row_from, app_settings_mail_from());
    autofill_smtp();
    (void)user;
}

static void mailpass_done(const char *text, void *user)
{
    if (text != NULL) {
        app_settings_set_mail_pass(text);
    }
    ui_list_row_set_value(s_row_mailpass,
                          app_settings_mail_pass()[0] ? "****" : "Not set");
    (void)user;
}

static void to_done(const char *text, void *user)
{
    if (text != NULL) {
        app_settings_set_mail_to(text);
    }
    ui_list_row_set_value(s_row_to, app_settings_mail_to());
    (void)user;
}

/*---------------------------------------------------------------------------
 * 发测试邮件
 *
 * 发送是阻塞的（TLS 握手 + 一轮 SMTP 对话要几秒），放自己的任务里跑，
 * 结果再 lv_async_call 回 LVGL 上下文。
 *-------------------------------------------------------------------------*/
static void mail_done_async(void *arg)
{
    app_mail_result_t r = (app_mail_result_t)(intptr_t)arg;
    if (s_row_testmail != NULL) {
        lv_obj_clear_state(s_row_testmail, LV_STATE_DISABLED);
    }
    ui_toast_show(app_mail_result_str(r),
                  r == APP_MAIL_OK ? lv_color_white() : UI_COLOR_DANGER);
}

static void mail_task(void *arg)
{
    (void)arg;
    app_mail_result_t r = app_mail_send_test();
    lv_async_call(mail_done_async, (void *)(intptr_t)r);
    vTaskDelete(NULL);
}

static void row_testmail_cb(lv_event_t *e)
{
    (void)e;

    /* 缺哪一项就直接说，别让人点了等半天再报"未配置" */
    const char *missing = app_mail_missing_field();
    if (missing != NULL) {
        char buf[48];
        snprintf(buf, sizeof(buf), "Set %s first", missing);
        ui_toast_show(buf, UI_COLOR_DANGER);
        return;
    }

    /* 立刻禁用，避免连点开出好几个任务 */
    if (s_row_testmail != NULL) {
        lv_obj_add_state(s_row_testmail, LV_STATE_DISABLED);
    }
    ui_toast_show("Sending...", lv_color_white());

    if (xTaskCreate(mail_task, "mail", 8192, NULL, 5, NULL) != pdPASS) {
        if (s_row_testmail != NULL) {
            lv_obj_clear_state(s_row_testmail, LV_STATE_DISABLED);
        }
        ui_toast_show("Out of memory", UI_COLOR_DANGER);
    }
}

/*---------------------------------------------------------------------------
 * 声音：快门声开关 + 音量
 *
 * **原来这里是三行测试工具**（Test Speaker 放 440Hz 长音、Shutter Sound 换音色
 * 试听、Speaker Bands 扫六个频点）。那是"快门声听不见"排查期间的临时产物，
 * 2026-09-22 定位到是素材问题之后按需求收成两行。
 *
 * 诊断能力没有丢：**拖动音量滑条、松手会放一声快门声**——既让你听清当前音量，
 * 也就顺带验了"I2S → ES8311 → 功放"这条通路还通不通（原来那是 Test Speaker
 * 的活）。要再扫频段的话，代码在 git 历史里。
 *
 * 这两个都**只存在内存里，掉电回到默认（开 / 50）**，跟音乐页那个音量一样。
 * 要落盘得给 app_settings 加键。
 *-------------------------------------------------------------------------*/
static lv_obj_t *s_sw_shutter;
static lv_obj_t *s_sl_vol;
static lv_obj_t *s_sw_rotate;

/* 定义在 refresh_status 那边（状态定时器附近），这里先用 */
static void refresh_rotate_status(void);

static void rotate_switch_cb(lv_event_t *e)
{
    lv_obj_t *sw = lv_event_get_target(e);
    const bool on = lv_obj_has_state(sw, LV_STATE_CHECKED);

    /* 立刻生效（翻转由 ui.c 那个 200ms 的定时器去设面板），再落盘：
     * 这是个"填一次就不动"的偏好，掉电该还在 */
    app_imu_set_enabled(on);
    if (app_settings_set_auto_rotate(on) != ESP_OK) {
        ui_toast_show("Could not save", UI_COLOR_DANGER);
    }
    /* 状态行跟着变：关掉时应该立刻显示 "Off"，而不是等下一拍定时器 */
    refresh_rotate_status();
}

static void shutter_switch_cb(lv_event_t *e)
{
    lv_obj_t *sw = lv_event_get_target(e);
    app_sound_set_shutter_enabled(lv_obj_has_state(sw, LV_STATE_CHECKED));
}

/*
 * 调音量的试听。放自己的任务里：app_sound_play_pcm() 会阻塞在 I2S 写入上
 * （写完还要等 DMA 排空），占着 LVGL 任务的话这段时间触摸整段丢。
 */
static void volume_preview_task(void *arg)
{
    (void)arg;
    /* 开关关着也要出声：这是一次明确的"我要听"的动作，不是拍照时的反馈 */
    app_sound_play_shutter_preview();
    vTaskDelete(NULL);
}

static void volume_changed_cb(lv_event_t *e)
{
    lv_obj_t *sl = lv_event_get_target(e);
    const int v = (int)lv_slider_get_value(sl);

    /*
     * **先把音频通路初始化出来再设音量**。bsp_audio_volume_set() 在没初始化时
     * 直接返回 ESP_ERR_INVALID_STATE，值既到不了 ES8311、也不会被 BSP 记下来
     * ——那样滑条拖了没反应，看着像滑条坏了。bsp_audio_init() 可重复调用。
     */
    if (bsp_audio_init() == ESP_OK) {
        app_audio_set_volume(v);
    }

    /*
     * 百分比那条值文本挂在**行**的 user_data 上（ui_list_row_create 放进去的），
     * 不是在滑条上——所以要先往上找一层。直接从滑条取会永远拿到 NULL，
     * 数字不动，而且不报错。
     */
    lv_obj_t *val = (lv_obj_t *)lv_obj_get_user_data(lv_obj_get_parent(sl));
    if (val != NULL) {
        lv_label_set_text_fmt(val, "%d", v);
    }

    if (lv_event_get_code(e) == LV_EVENT_VALUE_CHANGED) {
        return;                 /* 拖动中只跟值和数字，不反复出声 */
    }
    /* RELEASED：出声试听 */
    if (xTaskCreate(volume_preview_task, "vol", 3072, NULL, 6, NULL) != pdPASS) {
        ESP_LOGW(TAG, "no memory for volume preview task");
    }
}

/*
 * 音量那一行：左边 "Volume"、中间滑条、右边百分比。
 *
 * 滑条的宽度和偏移是量出来的（tools/measure_font.py）：行内容宽 280，
 * 标签 "Volume"(16px) 占 60、右侧三位数占 25，中间剩 170——取 150 留间距。
 */
static void add_volume_row(lv_obj_t *parent, int initial)
{
    lv_obj_t *row = ui_list_row_create(parent, "Volume", "", false);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_CLICKABLE);   /* 只有滑条可点 */
    lv_obj_t *val = (lv_obj_t *)lv_obj_get_user_data(row);
    if (val != NULL) {
        lv_label_set_text_fmt(val, "%d", initial);
    }

    s_sl_vol = lv_slider_create(row);
    lv_obj_set_size(s_sl_vol, 150, 18);
    lv_obj_align(s_sl_vol, LV_ALIGN_RIGHT_MID, -56, 0);
    lv_slider_set_range(s_sl_vol, 0, 100);
    lv_slider_set_value(s_sl_vol, initial, LV_ANIM_OFF);
    lv_obj_set_ext_click_area(s_sl_vol, 8);
    lv_obj_add_event_cb(s_sl_vol, volume_changed_cb, LV_EVENT_VALUE_CHANGED, NULL);
    lv_obj_add_event_cb(s_sl_vol, volume_changed_cb, LV_EVENT_RELEASED, NULL);
}

/*---------------------------------------------------------------------------
 * 行点击
 *-------------------------------------------------------------------------*/
static void row_ssid_cb(lv_event_t *e)
{
    (void)e;
    ui_scan_open(scan_picked, NULL);
}

static void row_pass_cb(lv_event_t *e)
{
    (void)e;
    ui_input_open("Wi-Fi Password", app_settings_wifi_pass(), true, false,
                  APP_SETTINGS_PASS_MAX, wifi_pass_done, NULL);
}

static void row_status_cb(lv_event_t *e)
{
    (void)e;
    /* 失败或未连接且有凭据时，点一下 = 手动重连 */
    if (app_settings_has_wifi() && !app_wifi_is_connected()) {
        app_wifi_retry();
    }
}

static void row_host_cb(lv_event_t *e)
{
    (void)e;
    ui_input_open("SMTP Server", app_settings_smtp_host(), false, false,
                  APP_SETTINGS_HOST_MAX, host_done, NULL);
}

static void row_port_cb(lv_event_t *e)
{
    (void)e;
    ui_input_open("SMTP Port", app_settings_smtp_port(), false, true,
                  APP_SETTINGS_PORT_MAX, port_done, NULL);
}

static void row_from_cb(lv_event_t *e)
{
    (void)e;
    ui_input_open("From Address", app_settings_mail_from(), false, false,
                  APP_SETTINGS_ADDR_MAX, from_done, NULL);
}

static void row_mailpass_cb(lv_event_t *e)
{
    (void)e;
    ui_input_open("App Password", app_settings_mail_pass(), true, false,
                  APP_SETTINGS_ADDR_MAX, mailpass_done, NULL);
}

static void row_to_cb(lv_event_t *e)
{
    (void)e;
    ui_input_open("To Address", app_settings_mail_to(), false, false,
                  APP_SETTINGS_ADDR_MAX, to_done, NULL);
}

/*---------------------------------------------------------------------------
 * 重置：lv_msgbox 二次确认
 *
 * 下面两个回调里都必须用 lv_event_get_current_target()，不能用
 * lv_event_get_target()：
 *   msgbox 的按钮是 btnmatrix，它发的 VALUE_CHANGED 会**冒泡**到 msgbox 才
 *   轮到回调。冒泡时 LVGL 只往上挪 current_target，e->target 始终是最初发出
 *   事件的对象（那个 btnmatrix）。用 get_target 拿到 btnmatrix 却被当成
 *   lv_msgbox_t 去读 btnm 成员，得到野指针后解引用，直接 LoadProhibited 复位。
 *-------------------------------------------------------------------------*/
static void reset_wifi_cb(lv_event_t *e)
{
    lv_obj_t *mbox = lv_event_get_current_target(e);
    if (lv_msgbox_get_active_btn(mbox) == 1) {
        app_settings_reset_wifi();
        app_wifi_disconnect();
        ui_list_row_set_value(s_row_ssid, "");
        ui_list_row_set_value(s_row_pass, "Not set");
        ui_list_row_set_value(s_row_status, app_wifi_status_str());
    }
    lv_msgbox_close_async(mbox);
}

static void reset_mail_cb(lv_event_t *e)
{
    lv_obj_t *mbox = lv_event_get_current_target(e);   /* 见上面 reset_wifi_cb 的说明 */
    if (lv_msgbox_get_active_btn(mbox) == 1) {
        app_settings_reset_mail();
        /* 自动填的记录也要一起清掉，否则下次改 From 时会被误判成
         * "这个值是我填的"而跳过覆盖 */
        s_autofilled_host[0] = '\0';
        ui_list_row_set_value(s_row_host, "");
        ui_list_row_set_value(s_row_port, "");
        ui_list_row_set_value(s_row_from, "");
        ui_list_row_set_value(s_row_mailpass, "Not set");
        ui_list_row_set_value(s_row_to, "");
    }
    lv_msgbox_close_async(mbox);
}

static void row_reset_wifi_cb(lv_event_t *e)
{
    (void)e;
    static const char *btns[] = { "No", "Yes", "" };
    lv_obj_t *mbox = lv_msgbox_create(NULL, "Reset Wi-Fi",
                                      "Erase saved Wi-Fi credentials?", btns, false);
    lv_obj_set_style_text_font(lv_msgbox_get_text(mbox), UI_FONT_SMALL, 0);
    lv_obj_add_event_cb(mbox, reset_wifi_cb, LV_EVENT_VALUE_CHANGED, NULL);
}

static void row_reset_mail_cb(lv_event_t *e)
{
    (void)e;
    static const char *btns[] = { "No", "Yes", "" };
    lv_obj_t *mbox = lv_msgbox_create(NULL, "Reset Email",
                                      "Erase saved email settings?", btns, false);
    lv_obj_set_style_text_font(lv_msgbox_get_text(mbox), UI_FONT_SMALL, 0);
    lv_obj_add_event_cb(mbox, reset_mail_cb, LV_EVENT_VALUE_CHANGED, NULL);
}

/*---------------------------------------------------------------------------
 * 定时刷新
 *-------------------------------------------------------------------------*/
static void refresh_status(void)
{
    if (s_row_status != NULL) {
        ui_list_row_set_value(s_row_status, app_wifi_status_str());
    }
}

/*
 * 自动翻转那一行的状态：传感器在不在、开关开没开、当前判成正立还是倒置。
 *
 * **这是给"串口日志看不见"留的后路**——用户把设备翻过来，盯着这一行就能
 * 判断功能到底有没有在工作，不用去翻串口。
 */
static void refresh_rotate_status(void)
{
    if (s_sw_rotate == NULL) {
        return;
    }
    lv_obj_t *row = (lv_obj_t *)lv_obj_get_user_data(s_sw_rotate);
    if (row != NULL) {
        ui_list_row_set_value(row, app_imu_status_str());
    }
}

static void refresh_storage(void);

static void status_timer_cb(lv_timer_t *t)
{
    (void)t;
    refresh_status();
    refresh_rotate_status();
    /* 剩余空间也跟着刷：从 U 盘模式退回来之后，卡刚被电脑写过，
     * 不刷的话这里会一直显示进模式之前的旧值 */
    if (s_row_free != NULL) {
        refresh_storage();
    }
}

static void refresh_storage(void)
{
    if (!app_storage_ready()) {
        ui_list_row_set_value(s_row_free, "No TF card");
        return;
    }
    uint64_t kb = app_storage_free_bytes() / 1024;
    static char buf[32];
    if (kb >= 1024 * 1024) {
        snprintf(buf, sizeof(buf), "%.1f GB", (double)kb / (1024 * 1024));
    } else if (kb >= 1024) {
        snprintf(buf, sizeof(buf), "%.1f MB", (double)kb / 1024);
    } else {
        snprintf(buf, sizeof(buf), "%llu KB", (unsigned long long)kb);
    }
    ui_list_row_set_value(s_row_free, buf);
}

/*---------------------------------------------------------------------------
 * 页面
 *-------------------------------------------------------------------------*/
lv_obj_t *page_settings_create(lv_obj_t *parent)
{
    /* 必须返回外壳本身，内容区用 ui_page_shell_content 取（见 ui_theme.h） */
    lv_obj_t *shell = ui_page_shell_create(parent, "Settings");
    lv_obj_t *content = ui_page_shell_content(shell);

    /* ---- Network ---- */
    add_section(content, "NETWORK");
    /* 点这一行开扫描列表，不再手打 SSID */
    s_row_ssid = add_row(content, "Network",
                         app_settings_wifi_ssid()[0] ? app_settings_wifi_ssid() : "",
                         true, row_ssid_cb, NULL);
    s_row_pass = add_row(content, "Password",
                         app_settings_wifi_pass()[0] ? "****" : "Not set",
                         true, row_pass_cb, NULL);
    s_row_status = add_row(content, "Status", app_wifi_status_str(),
                           false, row_status_cb, NULL);

    /* ---- Mail ----
     * From 放第一个：填完它，下面的 SMTP Server 会自动补上，
     * 从上往下填就是顺的。反过来先填服务器的话，用户得先知道服务器叫什么，
     * 而这个自动填功能存在的意义正是让他不用知道。 */
    add_section(content, "MAIL");
    s_row_from = add_row(content, "From", app_settings_mail_from(),
                         true, row_from_cb, NULL);
    s_row_host = add_row(content, "SMTP Server", app_settings_smtp_host(),
                         true, row_host_cb, NULL);
    s_row_port = add_row(content, "Port", app_settings_smtp_port(),
                         true, row_port_cb, NULL);
    s_row_mailpass = add_row(content, "App Password",
                             app_settings_mail_pass()[0] ? "****" : "Not set",
                             true, row_mailpass_cb, NULL);
    s_row_to = add_row(content, "To", app_settings_mail_to(),
                       true, row_to_cb, NULL);
    /* 填完之后总得有个地方验证这套配置真能用 */
    s_row_testmail = add_row(content, "Send Test Email", "",
                             true, row_testmail_cb, NULL);

    /* ---- Display ----
     * 自动翻转。**放设置页而不是工作模式页**：它改的是显示偏好（填一次就
     * 不动），不是"换一种工作方式"，而且它落盘、掉电还在——工作模式页那几项
     * 是刻意不落盘的（见 page_workmode.c） */
    add_section(content, "DISPLAY");
    s_sw_rotate = add_switch_row(content, "Auto Rotate", rotate_switch_cb);
    if (app_settings_auto_rotate()) {
        lv_obj_add_state(s_sw_rotate, LV_STATE_CHECKED);
    }
    refresh_rotate_status();
    ui_list_note_create(content,
                        "Turn the device upside down and the picture follows.\n"
                        "Uses the on-board accelerometer.");

    /* ---- Sound ----
     * 开关 + 音量两行，见上面那段注释（原来是三行测试工具） */
    add_section(content, "SOUND");

    s_sw_shutter = add_switch_row(content, "Shutter Sound", shutter_switch_cb);
    if (app_sound_shutter_enabled()) {
        lv_obj_add_state(s_sw_shutter, LV_STATE_CHECKED);
    }

    int vol = 50;
    app_audio_get_volume(&vol);
    add_volume_row(content, vol);
    ui_list_note_create(content, "Drag, then let go to hear it.");


    /* ---- Storage ---- */
    add_section(content, "STORAGE");
    s_row_free = add_row(content, "Free space", "", false, NULL, NULL);
    refresh_storage();

    /* ---- Reset ---- */
    add_section(content, "RESET");
    add_row(content, "Reset Wi-Fi", "", true, row_reset_wifi_cb, NULL);
    add_row(content, "Reset Email", "", true, row_reset_mail_cb, NULL);

    s_status_timer = lv_timer_create(status_timer_cb, 1000, NULL);
    ESP_LOGI(TAG, "settings page created");
    return shell;
}

void page_settings_destroy(void)
{
    /* 扫描层建在 lv_layer_top() 上，不归 ui.c 的页面生命周期管。
     * 不在这里关掉的话，扫描列表开着的时候退回主菜单，它会一直盖在上面 */
    ui_scan_close();

    if (s_status_timer != NULL) {
        lv_timer_del(s_status_timer);
        s_status_timer = NULL;
    }
    s_row_ssid = NULL;
    s_row_pass = NULL;
    s_row_status = NULL;
    s_row_host = NULL;
    s_row_port = NULL;
    s_row_from = NULL;
    s_row_mailpass = NULL;
    s_row_to = NULL;
    s_row_free = NULL;
    s_row_testmail = NULL;
    s_sw_shutter = NULL;
    s_sl_vol = NULL;
}
