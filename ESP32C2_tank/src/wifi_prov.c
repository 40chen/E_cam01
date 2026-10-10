/*
 * wifi_prov.c — 凭据存储 + STA 连接
 *
 * 凭据存在 NVS（namespace "tank"），Ecam 推一次就够，之后每次上电自己连。
 *
 * 连上之后要**主动把 IP 报给 Ecam**（link.c 里那条 `#ip`）：C2 没有屏幕，
 * 而网页遥控的地址只有 Ecam 的履带车页能显示出来——不报的话用户就只能在
 * 路由器后台里找它。
 */
#include "wifi_prov.h"
#include "esp_check.h"

#include <stdio.h>
#include <string.h>

#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "nvs.h"

static const char *TAG = "wifi";

#define NVS_NS      "tank"
#define KEY_SSID    "ssid"
#define KEY_PASS    "pass"

#define SSID_MAX    33
#define PASS_MAX    65

static char   s_ssid[SSID_MAX];
static char   s_pass[PASS_MAX];
static char   s_ip[16];
static char   s_err[32];
static bool   s_connected;
static bool   s_inited;

/* 连上/断开要通知 link.c 去发 `#ip`；用回调避免 wifi 去依赖 link */
static void (*s_ip_cb)(const char *ip);

void wifi_prov_set_ip_cb(void (*cb)(const char *ip))
{
    s_ip_cb = cb;
}

/*---------------------------------------------------------------------------
 * NVS
 *-------------------------------------------------------------------------*/
static void nvs_load(void)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) != ESP_OK) {
        return;
    }
    size_t len = sizeof(s_ssid);
    if (nvs_get_str(h, KEY_SSID, s_ssid, &len) != ESP_OK) {
        s_ssid[0] = '\0';
    }
    len = sizeof(s_pass);
    if (nvs_get_str(h, KEY_PASS, s_pass, &len) != ESP_OK) {
        s_pass[0] = '\0';
    }
    nvs_close(h);
    ESP_LOGI(TAG, "NVS: ssid=%s", s_ssid[0] ? s_ssid : "(空)");
}

static esp_err_t nvs_save(void)
{
    nvs_handle_t h;
    ESP_RETURN_ON_ERROR(nvs_open(NVS_NS, NVS_READWRITE, &h), TAG, "nvs_open");

    esp_err_t err = nvs_set_str(h, KEY_SSID, s_ssid);
    if (err == ESP_OK) {
        err = nvs_set_str(h, KEY_PASS, s_pass);
    }
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    nvs_close(h);
    return err;
}

/*---------------------------------------------------------------------------
 * 事件
 *-------------------------------------------------------------------------*/
static void on_wifi_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg; (void)base;
    switch (id) {
    case WIFI_EVENT_STA_START:
        if (s_ssid[0] != '\0') {
            esp_wifi_connect();
        }
        break;
    case WIFI_EVENT_STA_CONNECTED:
        s_err[0] = '\0';
        ESP_LOGI(TAG, "已连上 AP，等 DHCP");
        break;
    case WIFI_EVENT_STA_DISCONNECTED: {
        const wifi_event_sta_disconnected_t *d = (const wifi_event_sta_disconnected_t *)data;
        s_connected = false;
        s_ip[0] = '\0';
        /* 把原因记下来，Ecam 那边推凭据失败时能报具体是什么错（密码错、
         * 找不到 AP 是两回事，用户要能分清） */
        switch (d->reason) {
        /* 这三个都归到"密码/认证"：握手超时几乎总是密码错，
         * 用户看到的应该是"连不上这个网"而不是一个内部错误码 */
        case WIFI_REASON_AUTH_FAIL:
        case WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT:
        case WIFI_REASON_HANDSHAKE_TIMEOUT:
            strlcpy(s_err, "auth", sizeof(s_err));
            break;
        case WIFI_REASON_NO_AP_FOUND:
            strlcpy(s_err, "no ap", sizeof(s_err));
            break;
        default:
            snprintf(s_err, sizeof(s_err), "disc %d", d->reason);
            break;
        }
        ESP_LOGW(TAG, "断开：%s", s_err);
        if (s_ip_cb != NULL) {
            s_ip_cb("");            /* 掉线也报一次，让 Ecam 把地址清掉 */
        }
        /* 有凭据就一直重连。这里不做退避——车就停在用户脚边，
         * 断线重连越早越好 */
        if (s_ssid[0] != '\0') {
            esp_wifi_connect();
        }
        break;
    }
    default:
        break;
    }
}

static void on_ip_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg; (void)base;
    if (id != IP_EVENT_STA_GOT_IP) {
        return;
    }
    const ip_event_got_ip_t *e = (const ip_event_got_ip_t *)data;
    snprintf(s_ip, sizeof(s_ip), IPSTR, IP2STR(&e->ip_info.ip));
    s_connected = true;
    s_err[0] = '\0';
    ESP_LOGI(TAG, "IP = %s", s_ip);
    if (s_ip_cb != NULL) {
        s_ip_cb(s_ip);              /* 报给 Ecam，它显示在履带车页上 */
    }
}

/*---------------------------------------------------------------------------
 * 对外接口
 *-------------------------------------------------------------------------*/
esp_err_t wifi_prov_start(void)
{
    if (s_inited) {
        return ESP_OK;
    }

    nvs_load();

    ESP_RETURN_ON_ERROR(esp_netif_init(), TAG, "netif init");
    const esp_err_t ev = esp_event_loop_create_default();
    if (ev != ESP_OK && ev != ESP_ERR_INVALID_STATE) {
        return ev;
    }
    if (esp_netif_create_default_wifi_sta() == NULL) {
        return ESP_FAIL;
    }

    const wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_RETURN_ON_ERROR(esp_wifi_init(&cfg), TAG, "wifi init");
    ESP_RETURN_ON_ERROR(esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                                            on_wifi_event, NULL, NULL),
                        TAG, "wifi events");
    ESP_RETURN_ON_ERROR(esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                                            on_ip_event, NULL, NULL),
                        TAG, "ip events");

    ESP_RETURN_ON_ERROR(esp_wifi_set_mode(WIFI_MODE_STA), TAG, "set sta");
    if (s_ssid[0] != '\0') {
        wifi_config_t wc = { 0 };
        strlcpy((char *)wc.sta.ssid, s_ssid, sizeof(wc.sta.ssid));
        strlcpy((char *)wc.sta.password, s_pass, sizeof(wc.sta.password));
        ESP_RETURN_ON_ERROR(esp_wifi_set_config(WIFI_IF_STA, &wc), TAG, "set config");
    }
    ESP_RETURN_ON_ERROR(esp_wifi_start(), TAG, "wifi start");

    s_inited = true;
    return ESP_OK;
}

esp_err_t wifi_prov_set(const char *ssid, const char *pass)
{
    if (ssid == NULL || pass == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (strlen(ssid) >= SSID_MAX || strlen(pass) >= PASS_MAX) {
        return ESP_ERR_INVALID_ARG;     /* 超长：不截断，让上面明确报错 */
    }

    strlcpy(s_ssid, ssid, sizeof(s_ssid));
    strlcpy(s_pass, pass, sizeof(s_pass));

    ESP_RETURN_ON_ERROR(nvs_save(), TAG, "nvs save");

    if (!s_inited) {
        /* wifi 还没起来就先只存着，wifi_prov_start() 会用它 */
        return ESP_OK;
    }

    wifi_config_t wc = { 0 };
    strlcpy((char *)wc.sta.ssid, s_ssid, sizeof(wc.sta.ssid));
    strlcpy((char *)wc.sta.password, s_pass, sizeof(wc.sta.password));
    ESP_RETURN_ON_ERROR(esp_wifi_set_config(WIFI_IF_STA, &wc), TAG, "set config");

    /* 换网络要真断一次再连，否则 esp_wifi_connect() 可能直接返回
     * "already connected" 而什么都不做 */
    esp_wifi_disconnect();
    ESP_RETURN_ON_ERROR(esp_wifi_connect(), TAG, "connect");

    s_connected = false;
    s_ip[0] = '\0';
    s_err[0] = '\0';
    ESP_LOGI(TAG, "收到新凭据，开始连接 '%s'", s_ssid);
    return ESP_OK;
}

const char *wifi_prov_ip(void)
{
    return s_ip;
}

bool wifi_prov_connected(void)
{
    return s_connected;
}

const char *wifi_prov_last_error(void)
{
    return s_err;
}
