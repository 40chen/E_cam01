#include "app_settings.h"

#include <string.h>

#include "esp_log.h"
#include "nvs.h"
#include "nvs_flash.h"

static const char *TAG = "settings";

#define NVS_NAMESPACE "ecam"

/* 键名与 RAM 缓冲一一对应，一次 init 循环读完 */
typedef struct {
    const char *key;
    char       *buf;
    size_t      size;            /* 含结尾 NUL */
} setting_slot_t;

static char s_wifi_ssid[APP_SETTINGS_SSID_MAX + 1];
static char s_wifi_pass[APP_SETTINGS_PASS_MAX + 1];
static char s_smtp_host[APP_SETTINGS_HOST_MAX + 1];
static char s_smtp_port[APP_SETTINGS_PORT_MAX + 1];
static char s_mail_from[APP_SETTINGS_ADDR_MAX + 1];
static char s_mail_pass[APP_SETTINGS_ADDR_MAX + 1];
static char s_mail_to[APP_SETTINGS_ADDR_MAX + 1];
/*
 * 自动翻转（QMI8658 判方向）。**这是个开关而不是凭据**，但它是"填一次就不动"
 * 的用户偏好，所以落盘——不像副屏/U 盘那种"跟模式不跟设置"的东西。
 * 存成 "0"/"1" 是因为这张表里全是字符串，为它单开一个 nvs 类型不划算。
 */
static char s_auto_rotate[2] = "1";

static const setting_slot_t s_slots[] = {
    { "wifi_ssid", s_wifi_ssid, sizeof(s_wifi_ssid) },
    { "wifi_pass", s_wifi_pass, sizeof(s_wifi_pass) },
    { "smtp_host", s_smtp_host, sizeof(s_smtp_host) },
    { "smtp_port", s_smtp_port, sizeof(s_smtp_port) },
    { "mail_from", s_mail_from, sizeof(s_mail_from) },
    { "mail_pass", s_mail_pass, sizeof(s_mail_pass) },
    { "mail_to",   s_mail_to,   sizeof(s_mail_to)   },
    { "auto_rot",  s_auto_rotate, sizeof(s_auto_rotate) },
};

static esp_err_t set_str(const char *key, char *ram, size_t size, const char *val)
{
    if (val == NULL) {
        val = "";
    }
    /* 超长直接截断，比返回错误更符合"现场能用"的预期 */
    size_t len = strlen(val);
    if (len >= size) {
        len = size - 1;
        ESP_LOGW(TAG, "%s truncated to %u chars", key, (unsigned)len);
    }
    memcpy(ram, val, len);
    ram[len] = '\0';

    esp_err_t err = nvs_flash_init();   /* 幂等，主流程已初始化过 */
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nvs init failed: %s", esp_err_to_name(err));
        return err;
    }
    nvs_handle_t h;
    err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nvs_open failed: %s", esp_err_to_name(err));
        return err;
    }
    if (len == 0) {
        err = nvs_erase_key(h, key);
        if (err == ESP_ERR_NVS_NOT_FOUND) {
            err = ESP_OK;
        }
    } else {
        err = nvs_set_str(h, key, val);
    }
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    nvs_close(h);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nvs write %s failed: %s", key, esp_err_to_name(err));
    }
    return err;
}

esp_err_t app_settings_init(void)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READONLY, &h);
    if (err != ESP_OK) {
        /* 全新设备没有这个 namespace，不算错误 */
        ESP_LOGI(TAG, "no saved settings yet (%s)", esp_err_to_name(err));
        return err;
    }
    for (size_t i = 0; i < sizeof(s_slots) / sizeof(s_slots[0]); i++) {
        size_t len = s_slots[i].size;
        err = nvs_get_str(h, s_slots[i].key, s_slots[i].buf, &len);
        if (err == ESP_ERR_NVS_NOT_FOUND) {
            s_slots[i].buf[0] = '\0';
        } else if (err != ESP_OK) {
            s_slots[i].buf[0] = '\0';
            ESP_LOGW(TAG, "read %s failed: %s", s_slots[i].key, esp_err_to_name(err));
        }
    }
    nvs_close(h);
    ESP_LOGI(TAG, "loaded: wifi=%s mail=%s",
             app_settings_has_wifi() ? "set" : "none",
             app_settings_has_mail() ? "set" : "none");
    return ESP_OK;
}

/* ---- getter ---- */
const char *app_settings_wifi_ssid(void) { return s_wifi_ssid; }
const char *app_settings_wifi_pass(void) { return s_wifi_pass; }
const char *app_settings_smtp_host(void) { return s_smtp_host; }
const char *app_settings_smtp_port(void) { return s_smtp_port; }
const char *app_settings_mail_from(void) { return s_mail_from; }
const char *app_settings_mail_pass(void) { return s_mail_pass; }
const char *app_settings_mail_to(void)   { return s_mail_to; }

bool app_settings_has_wifi(void) { return s_wifi_ssid[0] != '\0'; }
bool app_settings_has_mail(void) { return s_mail_from[0] != '\0'; }

bool app_settings_auto_rotate(void)
{
    /*
     * **默认是开，但"开"不能靠 s_auto_rotate 的初值**：上面那个读循环对所有
     * 槽位一视同仁，键不存在（全新设备）时会把缓冲清成空串，"1" 那个初值
     * 早就没了。所以判据是"只有明确存过 0 才算关"——空串、"1"、任何别的
     * 东西都当开。
     */
    return s_auto_rotate[0] != '0';
}

/* ---- setter ---- */
esp_err_t app_settings_set_auto_rotate(bool on)
{
    return set_str("auto_rot", s_auto_rotate, sizeof(s_auto_rotate), on ? "1" : "0");
}

esp_err_t app_settings_set_wifi_ssid(const char *val)
{
    return set_str("wifi_ssid", s_wifi_ssid, sizeof(s_wifi_ssid), val);
}
esp_err_t app_settings_set_wifi_pass(const char *val)
{
    return set_str("wifi_pass", s_wifi_pass, sizeof(s_wifi_pass), val);
}
esp_err_t app_settings_set_smtp_host(const char *val)
{
    return set_str("smtp_host", s_smtp_host, sizeof(s_smtp_host), val);
}
esp_err_t app_settings_set_smtp_port(const char *val)
{
    return set_str("smtp_port", s_smtp_port, sizeof(s_smtp_port), val);
}
esp_err_t app_settings_set_mail_from(const char *val)
{
    return set_str("mail_from", s_mail_from, sizeof(s_mail_from), val);
}
esp_err_t app_settings_set_mail_pass(const char *val)
{
    return set_str("mail_pass", s_mail_pass, sizeof(s_mail_pass), val);
}
esp_err_t app_settings_set_mail_to(const char *val)
{
    return set_str("mail_to", s_mail_to, sizeof(s_mail_to), val);
}

/* ---- reset ---- */
esp_err_t app_settings_reset_wifi(void)
{
    esp_err_t err = set_str("wifi_ssid", s_wifi_ssid, sizeof(s_wifi_ssid), "");
    if (err == ESP_OK) {
        err = set_str("wifi_pass", s_wifi_pass, sizeof(s_wifi_pass), "");
    }
    return err;
}

esp_err_t app_settings_reset_mail(void)
{
    esp_err_t err = set_str("smtp_host", s_smtp_host, sizeof(s_smtp_host), "");
    if (err == ESP_OK) {
        err = set_str("smtp_port", s_smtp_port, sizeof(s_smtp_port), "");
    }
    if (err == ESP_OK) {
        err = set_str("mail_from", s_mail_from, sizeof(s_mail_from), "");
    }
    if (err == ESP_OK) {
        err = set_str("mail_pass", s_mail_pass, sizeof(s_mail_pass), "");
    }
    if (err == ESP_OK) {
        err = set_str("mail_to", s_mail_to, sizeof(s_mail_to), "");
    }
    return err;
}
