#include "app_wifi.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_netif_sntp.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "wifi";

#define MAX_RETRY        5          /* 断开后最多自动重连次数 */

/*
 * 对时。时钟不准的话 TLS 校验证书有效期会失败，发邮件就走不了，所以这一步
 * 的可靠性直接影响邮件功能。
 *
 * 两个坑：
 *   1. 只配 pool.ntp.org 在国内经常解析慢或者干脆不可达。放国内可达性好的
 *      服务器在前，lwIP 会并发查询这些服务器、取最先回的。
 *   2. 原来的实现超时之后就再也不试了（sntp_start_once 只在拿到 IP 时调
 *      一次），一次网络抖动就把邮件功能钉死到下次 WiFi 重连。现在超时会
 *      再等一轮——SNTP 客户机本身按自己的周期在反复请求，我们再调一次
 *      sync_wait 就是在等它下一轮的结果，不需要重新 init。
 */
#define SNTP_SERVER_1    "ntp.aliyun.com"
#define SNTP_SERVER_2    "cn.pool.ntp.org"
#define SNTP_SERVER_3    "pool.ntp.org"
#define SNTP_TIMEOUT_MS  15000      /* 每轮等多久 */
#define SNTP_RETRY_WAIT_MS 10000    /* 一轮失败后歇多久再等下一轮 */
#define SNTP_MAX_ATTEMPTS  4

/*---------------------------------------------------------------------------
 * 状态
 *-------------------------------------------------------------------------*/
static app_wifi_status_t s_status = APP_WIFI_IDLE;
static char s_ssid[33];
static char s_pass[65];
static char s_ip[16];
static char s_fail_reason[24];      /* 最后一次失败的可读原因 */
static bool s_wifi_started = false;
static bool s_want_connect = false;
static bool s_sntp_running = false;
static int  s_retry_left = 0;

/*
 * 扫描进行中。这个标志只有一个作用，但是必须的：扫描要挨个信道跳，
 * 跳信道时驱动会上报 WIFI_EVENT_STA_DISCONNECTED。事件处理里看到它
 * 就直接返回，既不累计失败原因也不触发重连——否则扫描会被自己的重连
 * 打断，而且连接正常时扫一次网就会把五次重试配额白白耗光。
 */
static bool s_scanning = false;

/*
 * 扫描期间收到过断开事件。扫描结束时靠它决定要不要补一次连接：
 * 只在 s_status 上判断是不够的——连着的时候被扫描搞掉线，守卫把事件吃掉，
 * s_status 还停在 CONNECTED，状态行就会一直显示"已连接"骗人。
 */
static bool s_scan_saw_disconnect = false;

/*---------------------------------------------------------------------------
 * 内部工具
 *-------------------------------------------------------------------------*/
static const char *reason_str(uint8_t reason)
{
    switch (reason) {
    case WIFI_REASON_AUTH_FAIL:                 return "Auth failed";
    case WIFI_REASON_ASSOC_FAIL:                return "Assoc failed";
    case WIFI_REASON_NO_AP_FOUND:               return "No AP found";
    case WIFI_REASON_BEACON_TIMEOUT:            return "AP out of range";
    case WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT:    return "Handshake timeout";
    case WIFI_REASON_HANDSHAKE_TIMEOUT:         return "Handshake timeout";
    default:                                    return "Disconnected";
    }
}

/* 认证类失败重试没有意义：密码错了再连多少次都是错 */
static bool reason_is_auth(uint8_t reason)
{
    return reason == WIFI_REASON_AUTH_FAIL ||
           reason == WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT ||
           reason == WIFI_REASON_HANDSHAKE_TIMEOUT;
}

static void apply_sta_config(void)
{
    wifi_config_t cfg = { 0 };
    strlcpy((char *)cfg.sta.ssid, s_ssid, sizeof(cfg.sta.ssid));
    strlcpy((char *)cfg.sta.password, s_pass, sizeof(cfg.sta.password));

    /*
     * threshold.authmode 不只是"最低安全要求"，它同时是驱动内部扫描的
     * 过滤条件：低于这个门槛的 AP 会被直接滤掉，表现是永远 "No AP found"。
     * 所以空密码（开放网络）必须把门槛放到 OPEN，否则扫描列表里选中的
     * 那个开放 AP 根本连不上。
     * 有密码时用 WPA_PSK：WPA 和 WPA2 都能连（写死 WPA2 会把只支持 WPA
     * 的老路由器挡在外面），WEP 仍然拒绝。
     */
    cfg.sta.threshold.authmode = (s_pass[0] == '\0') ? WIFI_AUTH_OPEN : WIFI_AUTH_WPA_PSK;
    cfg.sta.pmf_cfg.capable = true;
    cfg.sta.pmf_cfg.required = false;
    esp_wifi_set_config(WIFI_IF_STA, &cfg);
}

/*---------------------------------------------------------------------------
 * SNTP：拿到 IP 后拉起一个短命任务完成对时，不占事件循环
 *-------------------------------------------------------------------------*/
static void sntp_task(void *arg)
{
    (void)arg;

    esp_sntp_config_t cfg = ESP_NETIF_SNTP_DEFAULT_CONFIG_MULTIPLE(3,
        ESP_SNTP_SERVER_LIST(SNTP_SERVER_1, SNTP_SERVER_2, SNTP_SERVER_3));
    esp_err_t err = esp_netif_sntp_init(&cfg);
    if (err != ESP_OK) {
        /* 已经初始化过（比如 WiFi 中途重连又走到这里）不算错，接着等就行 */
        ESP_LOGW(TAG, "SNTP init: %s (continuing)", esp_err_to_name(err));
    }

    for (int attempt = 1; attempt <= SNTP_MAX_ATTEMPTS; attempt++) {
        err = esp_netif_sntp_sync_wait(pdMS_TO_TICKS(SNTP_TIMEOUT_MS));
        if (err == ESP_OK) {
            time_t now = time(NULL);
            struct tm t;
            localtime_r(&now, &t);
            ESP_LOGI(TAG, "SNTP synced: %04d-%02d-%02d %02d:%02d:%02d",
                     t.tm_year + 1900, t.tm_mon + 1, t.tm_mday,
                     t.tm_hour, t.tm_min, t.tm_sec);
            break;
        }
        ESP_LOGW(TAG, "SNTP 第 %d/%d 轮没等到回应 (%s)",
                 attempt, SNTP_MAX_ATTEMPTS, esp_err_to_name(err));
        if (attempt < SNTP_MAX_ATTEMPTS) {
            /* 客户机还在按自己的周期请求，歇一下再等下一轮结果 */
            vTaskDelay(pdMS_TO_TICKS(SNTP_RETRY_WAIT_MS));
        }
    }

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "对时一直没成功——发邮件会一直提示 Waiting for clock sync。"
                 "先确认这个网络能上外网、DNS 正常");
    }
    s_sntp_running = false;
    vTaskDelete(NULL);
}

static void sntp_start_once(void)
{
    if (s_sntp_running) {
        return;
    }
    s_sntp_running = true;
    if (xTaskCreate(sntp_task, "sntp", 3072, NULL, 5, NULL) != pdPASS) {
        s_sntp_running = false;
        ESP_LOGW(TAG, "SNTP task create failed");
    }
}

/*---------------------------------------------------------------------------
 * WiFi / IP 事件
 *-------------------------------------------------------------------------*/
static void event_handler(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;

    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        s_wifi_started = true;
        if (s_want_connect) {
            apply_sta_config();
            s_status = APP_WIFI_CONNECTING;
            s_retry_left = MAX_RETRY;
            ESP_LOGW(TAG, "connecting to \"%s\"", s_ssid);
            esp_wifi_connect();
        }
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        wifi_event_sta_disconnected_t *evt = data;
        const char *why = reason_str(evt->reason);

        if (s_scanning) {
            /* 这是扫描跳信道引起的，不是连接失败：不动 s_retry_left，
             * 也不重连（重连会把扫描打断）。只记一笔，扫描任务收尾时补连。 */
            s_scan_saw_disconnect = true;
            ESP_LOGD(TAG, "disconnect during scan (%s), deferred", why);
            return;
        }
        if (!s_want_connect) {
            s_status = APP_WIFI_IDLE;
            return;
        }
        if (reason_is_auth(evt->reason)) {
            s_status = APP_WIFI_FAILED;
            s_retry_left = 0;
            strlcpy(s_fail_reason, why, sizeof(s_fail_reason));
            ESP_LOGE(TAG, "%s — giving up (wrong password?)", why);
            return;
        }
        if (s_retry_left > 0) {
            s_retry_left--;
            s_status = APP_WIFI_CONNECTING;
            strlcpy(s_fail_reason, why, sizeof(s_fail_reason));
            ESP_LOGW(TAG, "%s, retrying (%d left)", why, s_retry_left);
            /* 重连前重放配置：可能在"已连状态下换新凭据"的场景里
             * 刚 set 了新配置，这里确保它生效 */
            apply_sta_config();
            vTaskDelay(pdMS_TO_TICKS(1000));
            /* 退避这一秒里用户可能打开了扫描列表。这时再 esp_wifi_connect()
             * 会把扫描掐掉（驱动文档：扫描和连接同时进行会中止扫描并返回
             * ESP_ERR_WIFI_STATE）。让扫描任务收尾时去补这次连接。 */
            if (s_scanning) {
                ESP_LOGW(TAG, "scan started during retry backoff, deferring connect");
                return;
            }
            esp_wifi_connect();
        } else {
            s_status = APP_WIFI_FAILED;
            ESP_LOGE(TAG, "%s — retries exhausted", why);
        }
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *evt = data;
        esp_ip4addr_ntoa(&evt->ip_info.ip, s_ip, sizeof(s_ip));
        s_status = APP_WIFI_CONNECTED;
        s_retry_left = 0;
        ESP_LOGI(TAG, "connected, ip=%s", s_ip);
        sntp_start_once();
    } else if (base == IP_EVENT && id == IP_EVENT_STA_LOST_IP) {
        s_ip[0] = '\0';
    }
}

/*---------------------------------------------------------------------------
 * 扫描
 *-------------------------------------------------------------------------*/

/* 强的排前面。qsort 的比较函数在文件作用域，不能写进函数里 */
static int rssi_cmp_desc(const void *a, const void *b)
{
    int ra = ((const app_wifi_ap_t *)a)->rssi;
    int rb = ((const app_wifi_ap_t *)b)->rssi;
    return rb - ra;
}

/*
 * mesh / 双频路由会把同一个 SSID 广播成好几个 BSSID，列表里出现四行
 * 一模一样的名字根本没法选。排好序（RSSI 降序）之后再走一遍，每个
 * SSID 只留第一个，也就是最强的那条。
 */
static int dedup_same_ssid(app_wifi_ap_t *aps, int n)
{
    int out = 0;
    for (int i = 0; i < n; i++) {
        bool seen = false;
        for (int j = 0; j < out; j++) {
            if (strcmp(aps[i].ssid, aps[j].ssid) == 0) {
                seen = true;
                break;
            }
        }
        if (!seen) {
            aps[out++] = aps[i];
        }
    }
    return out;
}

static void scan_task(void *arg)
{
    app_wifi_scan_cb_t cb = (app_wifi_scan_cb_t)arg;
    app_wifi_ap_t aps[APP_WIFI_SCAN_MAX];
    int count = 0;
    esp_err_t err = ESP_OK;

    /* esp_wifi_scan_start 在 wifi 还在 connecting 时返回 ESP_ERR_WIFI_STATE。
     * 连接尝试要么很快成功要么很快失败，等一小会儿就够了，不用取消它。 */
    for (int i = 0; i < 10 && s_status == APP_WIFI_CONNECTING; i++) {
        vTaskDelay(pdMS_TO_TICKS(200));
    }

    /* s_scanning / s_scan_saw_disconnect 已在 app_wifi_scan() 里置好，
     * 那里比这里更早，中间那两秒等待窗口里的事件也不会漏 */

    /* active.max 单位是毫秒，头文件写明上限 1500，超过会把连接搞断。
     * 300ms 比默认的 120ms 更容易扫到弱信号，全信道也就多花两秒多。 */
    wifi_scan_config_t cfg = {
        .show_hidden = false,
        .scan_time.active.max = 300,
    };
    err = esp_wifi_scan_start(&cfg, true);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "scan failed: %s", esp_err_to_name(err));
        goto done;
    }

    uint16_t num = 0;
    if (esp_wifi_scan_get_ap_num(&num) != ESP_OK) {
        err = ESP_FAIL;
        goto done;
    }
    ESP_LOGI(TAG, "scan done: %u AP(s)", (unsigned)num);
    if (num == 0) {
        goto done;
    }

    /*
     * 驱动把扫描结果存在自己动态分配的内存里，这份内存只能靠
     * esp_wifi_scan_get_ap_records() 或 esp_wifi_clear_ap_list() 释放。
     * 头文件特别注明：**没把记录全部取走**就必须调 clear_ap_list。
     * 所以这里按 num 全量取，截断留到下面往 aps[] 里拷贝时再做。
     */
    wifi_ap_record_t *recs = calloc(num, sizeof(*recs));
    if (recs == NULL) {
        err = ESP_ERR_NO_MEM;
        esp_wifi_clear_ap_list();
        goto done;
    }
    if (esp_wifi_scan_get_ap_records(&num, recs) != ESP_OK) {
        err = ESP_FAIL;
        free(recs);
        /* 取失败等于"没取完"，按头文件的约定这里要显式清一次驱动那份内存。
         * 上面成功取全量那条路径不能再调，会和 get_ap_records 重复释放。 */
        esp_wifi_clear_ap_list();
        goto done;
    }

    for (int i = 0; i < (int)num && count < APP_WIFI_SCAN_MAX; i++) {
        if (recs[i].ssid[0] == '\0') {
            continue;               /* 没有 SSID 就配不出连接，跳过 */
        }
        strlcpy(aps[count].ssid, (const char *)recs[i].ssid, sizeof(aps[count].ssid));
        aps[count].rssi       = recs[i].rssi;
        aps[count].needs_pass = (recs[i].authmode != WIFI_AUTH_OPEN);
        count++;
    }
    free(recs);

    qsort(aps, count, sizeof(aps[0]), rssi_cmp_desc);
    count = dedup_same_ssid(aps, count);

done:
    s_scanning = false;

    /*
     * 扫描期间压掉的那次重连在这里补回来。两个条件缺一不可：
     *   - s_scan_saw_disconnect：扫描把已经连上的链路搞掉了
     *   - s_status != CONNECTED：扫描开始时正在连接/退避，被打断在半路
     */
    if (s_want_connect &&
        (s_scan_saw_disconnect || s_status != APP_WIFI_CONNECTED)) {
        apply_sta_config();
        s_status = APP_WIFI_CONNECTING;
        ESP_LOGI(TAG, "resuming connect to \"%s\"", s_ssid);
        esp_wifi_connect();
    }
    s_scan_saw_disconnect = false;

    if (cb != NULL) {
        cb(aps, count, err);
    }
    vTaskDelete(NULL);
}

esp_err_t app_wifi_scan(app_wifi_scan_cb_t cb)
{
    if (!s_wifi_started) {
        return ESP_ERR_WIFI_NOT_STARTED;
    }
    if (s_scanning) {
        return ESP_ERR_INVALID_STATE;
    }
    /*
     * 任务创建前就把这两个标志置好，而且要早于任务里的 esp_wifi_scan_start：
     * 跳信道产生的断开事件全靠 s_scanning 挡掉；放在任务里置位的话，
     * 任务启动前那一小段仍然会走正常的重连逻辑，把扫描掐掉。
     */
    s_scanning = true;
    s_scan_saw_disconnect = false;
    if (xTaskCreate(scan_task, "wifi_scan", 4096, (void *)cb, 5, NULL) != pdPASS) {
        s_scanning = false;
        ESP_LOGE(TAG, "scan task create failed");
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

bool app_wifi_is_scanning(void)
{
    return s_scanning;
}

/*---------------------------------------------------------------------------
 * 对外 API
 *-------------------------------------------------------------------------*/
esp_err_t app_wifi_init(void)
{
    esp_err_t err = esp_netif_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "netif init failed: %s", esp_err_to_name(err));
        return err;
    }
    err = esp_event_loop_create_default();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "event loop failed: %s", esp_err_to_name(err));
        return err;
    }
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    err = esp_wifi_init(&cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "wifi init failed: %s", esp_err_to_name(err));
        return err;
    }

    /* 凭据由 app_settings 存在 NVS，不要让 WiFi 驱动再存一份 */
    esp_wifi_set_storage(WIFI_STORAGE_RAM);

    /* 注册失败不中止启动（项目容错原则），只是 WiFi 用不了 */
    err = esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, event_handler, NULL);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "wifi event register failed: %s", esp_err_to_name(err));
        return err;
    }
    err = esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, event_handler, NULL);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "ip event register failed: %s", esp_err_to_name(err));
        return err;
    }
    err = esp_event_handler_register(IP_EVENT, IP_EVENT_STA_LOST_IP, event_handler, NULL);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "ip lost event register failed: %s", esp_err_to_name(err));
        return err;
    }

    esp_wifi_set_mode(WIFI_MODE_STA);
    err = esp_wifi_start();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "wifi start failed: %s", esp_err_to_name(err));
    }
    return err;
}

esp_err_t app_wifi_connect(const char *ssid, const char *pass)
{
    if (ssid == NULL || ssid[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }
    app_wifi_status_t prev = s_status;
    strlcpy(s_ssid, ssid, sizeof(s_ssid));
    strlcpy(s_pass, pass ? pass : "", sizeof(s_pass));
    s_want_connect = true;
    s_retry_left = MAX_RETRY;

    if (!s_wifi_started) {
        /* STA_START 事件会接上这一步 */
        s_status = APP_WIFI_CONNECTING;
        return ESP_OK;
    }
    apply_sta_config();
    s_status = APP_WIFI_CONNECTING;
    ESP_LOGI(TAG, "connecting to \"%s\"", s_ssid);

    if (prev == APP_WIFI_CONNECTED || prev == APP_WIFI_CONNECTING) {
        /* 已连/在连状态下换凭据：先断开，DISCONNECTED 事件会带着新配置重连 */
        esp_wifi_disconnect();
        return ESP_OK;
    }
    esp_err_t err = esp_wifi_connect();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_wifi_connect failed: %s", esp_err_to_name(err));
        s_status = APP_WIFI_FAILED;
        strlcpy(s_fail_reason, "Connect failed", sizeof(s_fail_reason));
    }
    return err;
}

esp_err_t app_wifi_disconnect(void)
{
    s_want_connect = false;
    s_retry_left = 0;
    s_ip[0] = '\0';
    if (s_wifi_started) {
        esp_wifi_disconnect();      /* 已断开时返回错误，忽略 */
    }
    s_status = APP_WIFI_IDLE;
    ESP_LOGI(TAG, "disconnected by user");
    return ESP_OK;
}

esp_err_t app_wifi_retry(void)
{
    /* 用上次连接时留下的凭据副本再试 */
    if (s_ssid[0] == '\0') {
        return ESP_ERR_INVALID_STATE;
    }
    return app_wifi_connect(s_ssid, s_pass);
}

app_wifi_status_t app_wifi_status(void)
{
    return s_status;
}

bool app_wifi_is_connected(void)
{
    return s_status == APP_WIFI_CONNECTED;
}

const char *app_wifi_status_str(void)
{
    static char buf[40];
    switch (s_status) {
    case APP_WIFI_CONNECTED:
        snprintf(buf, sizeof(buf), "Connected %s", s_ip);
        break;
    case APP_WIFI_CONNECTING:
        snprintf(buf, sizeof(buf), "Connecting...");
        break;
    case APP_WIFI_FAILED:
        snprintf(buf, sizeof(buf), "%s", s_fail_reason[0] ? s_fail_reason : "Failed");
        break;
    case APP_WIFI_IDLE:
    default:
        snprintf(buf, sizeof(buf), "Not configured");
        break;
    }
    return buf;
}
