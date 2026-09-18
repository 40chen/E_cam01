/*
 * page_settings.c — 设置页
 *
 * 分组列表：Network（WiFi 凭据 + 实时状态）/ Mail（SMTP 与邮箱）/ Storage
 * （只读）/ Reset（带二次确认的擦除）。凭据通过 ui_input 软键盘输入，
 * 存 NVS（app_settings），保存 SSID 后立即发起连接（app_wifi）。
 *
 * 状态行用 lv_timer 每秒刷新，和 page_home.c 刷 TF 卡图标同一套路。
 */
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "app_mail.h"
#include "app_settings.h"
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
static lv_obj_t *s_row_tone = NULL;
static lv_timer_t *s_status_timer = NULL;

/*---------------------------------------------------------------------------
 * 小工具
 *-------------------------------------------------------------------------*/
static lv_obj_t *add_section(lv_obj_t *parent, const char *title)
{
    lv_obj_t *lbl = lv_label_create(parent);
    lv_label_set_text(lbl, title);
    lv_obj_set_style_text_font(lbl, UI_FONT_SMALL, 0);
    lv_obj_set_style_text_color(lbl, UI_COLOR_TEXT_SUB, 0);
    /* 组间空隙用 pad_top 做（LVGL 8 没有 margin） */
    lv_obj_set_style_pad_top(lbl, 8, 0);
    return lbl;
}

static lv_obj_t *add_row(lv_obj_t *parent, const char *label, const char *value,
                         bool arrow, lv_event_cb_t cb, void *user)
{
    lv_obj_t *row = ui_list_row_create(parent, label, value, arrow);
    lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
    if (cb != NULL) {
        lv_obj_add_event_cb(row, cb, LV_EVENT_CLICKED, user);
    }
    return row;
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
 * 测试扬声器
 *
 * 放 1.5 秒 440Hz 正弦，**运行时生成**，不占 flash。单声道复制成两声道。
 *
 * 这一行存在的意义是把"没声音"这件事一次性分成两半：1.5 秒的长音如果又响
 * 又完整，说明 I2S → ES8311 → 功放整条通路是通的，问题只可能在音效素材；
 * 如果它也只是短短一下、而且改音量也毫无反应，那通路是死的——那种"短短
 * 一下"是功放使能（PCA9557 bit1）的开关瞬态，跟 PCM 内容毫无关系。
 *-------------------------------------------------------------------------*/
#define TEST_TONE_HZ     440
#define TEST_TONE_MS     1500
/* 一块 4096 帧 ≈ 256ms。必须明显大于 I2S 的 DMA 环形缓冲（约 90ms），
 * 否则每块之间会断一下，听起来是"嘟嘟嘟"而不是连续的长音 */
#define TEST_TONE_CHUNK  4096
#define TEST_TONE_AMP    19660          /* int16 满量程的 60% */

static void tone_done_async(void *arg)
{
    esp_err_t err = (esp_err_t)(intptr_t)arg;
    if (s_row_tone != NULL) {
        lv_obj_clear_state(s_row_tone, LV_STATE_DISABLED);
    }
    ui_toast_show(err == ESP_OK ? "Test tone done" : esp_err_to_name(err),
                  err == ESP_OK ? lv_color_white() : UI_COLOR_DANGER);
}

static void test_tone_task(void *arg)
{
    (void)arg;
    esp_err_t result = ESP_FAIL;

    /* 用户可能压根没进过取景页，音频从没初始化过。bsp_audio_init 可重复调用 */
    esp_err_t aerr = bsp_audio_init();
    if (aerr != ESP_OK) {
        ESP_LOGE(TAG, "audio init failed: %s", esp_err_to_name(aerr));
        lv_async_call(tone_done_async, (void *)(intptr_t)aerr);
        vTaskDelete(NULL);
        return;
    }

    const int chunk_bytes = TEST_TONE_CHUNK * 2 * (int)sizeof(int16_t);
    int16_t *buf = heap_caps_malloc(chunk_bytes, MALLOC_CAP_DMA);
    if (buf == NULL) {
        ESP_LOGE(TAG, "no DMA mem for test tone (%d bytes)", chunk_bytes);
        lv_async_call(tone_done_async, (void *)(intptr_t)ESP_ERR_NO_MEM);
        vTaskDelete(NULL);
        return;
    }

    const int total = (int)((int64_t)BSP_AUDIO_DEFAULT_RATE * TEST_TONE_MS / 1000);
    const double step = 2.0 * M_PI * TEST_TONE_HZ / BSP_AUDIO_DEFAULT_RATE;
    double phase = 0.0;
    int done = 0;
    size_t total_written = 0;

    esp_err_t paerr = bsp_pa_en(1);
    while (done < total) {
        int n = total - done;
        if (n > TEST_TONE_CHUNK) {
            n = TEST_TONE_CHUNK;
        }
        for (int i = 0; i < n; i++) {
            int16_t s = (int16_t)(sin(phase) * TEST_TONE_AMP);
            phase += step;
            if (phase > 2.0 * M_PI) {
                phase -= 2.0 * M_PI;
            }
            buf[i * 2] = s;
            buf[i * 2 + 1] = s;
        }
        size_t w = 0;
        esp_err_t err = bsp_audio_write(buf, (size_t)n * 4, &w, 2000);
        total_written += w;
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "test tone write failed at %d/%d frames: %s",
                     done, total, esp_err_to_name(err));
            break;
        }
        done += n;
    }
    /* 写入返回时 DMA 里还剩一圈没放完，等它出来再关功放 */
    vTaskDelay(pdMS_TO_TICKS(120));
    bsp_pa_en(0);

    /*
     * 这一行是排查"没声音"最有价值的输出：真实写进 I2S 的字节数。
     *   wrote 对得上但还是没声音 → 问题在 I2S 之后（ES8311 / 功放）
     *   wrote 对不上             → 问题在写入这一步
     */
    ESP_LOGI(TAG, "test tone: %d Hz %d ms | wrote %u/%u bytes | pa1=%s",
             TEST_TONE_HZ, TEST_TONE_MS,
             (unsigned)total_written, (unsigned)((size_t)total * 4),
             esp_err_to_name(paerr));

    result = (done >= total) ? ESP_OK : ESP_FAIL;
    free(buf);
    lv_async_call(tone_done_async, (void *)(intptr_t)result);
    vTaskDelete(NULL);
}

static void row_tone_cb(lv_event_t *e)
{
    (void)e;
    if (s_row_tone != NULL) {
        lv_obj_add_state(s_row_tone, LV_STATE_DISABLED);   /* 防连点 */
    }
    ui_toast_show("Playing...", lv_color_white());
    if (xTaskCreate(test_tone_task, "tone", 4096, NULL, 6, NULL) != pdPASS) {
        if (s_row_tone != NULL) {
            lv_obj_clear_state(s_row_tone, LV_STATE_DISABLED);
        }
        ui_toast_show("Out of memory", UI_COLOR_DANGER);
    }
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

static void status_timer_cb(lv_timer_t *t)
{
    (void)t;
    refresh_status();
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

    /* ---- Sound ----
     * 一个能直接验证喇叭通不通的地方。也用来区分"音效素材太轻"和
     * "整条音频通路没接通"——见 test_tone_task 的注释 */
    add_section(content, "SOUND");
    s_row_tone = add_row(content, "Test Speaker", "", true, row_tone_cb, NULL);

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
    s_row_tone = NULL;
}
