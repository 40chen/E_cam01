/*
 * app_mail.c — SMTP 客户端实现
 *
 * 协议本身很简单：一条命令一行应答，AUTH LOGIN 之后 MAIL FROM / RCPT TO /
 * DATA / QUIT。真正会咬人的是两个地方，都在下面单独标注了：
 *   1. TLS 校验证书要用系统时间，而时间靠 SNTP 在连上 WiFi 之后才有；
 *   2. 应答可能是多行（"250-xxx" 若干行 + "250 xxx" 收尾），只读一行会错位。
 *
 * 日志里只记回复码，绝不记密码和 base64 凭证。
 */
#include "app_mail.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>            /* strcasecmp */
#include <sys/stat.h>           /* stat，取附件大小 */
#include <time.h>

#include "esp_crt_bundle.h"
#include "esp_log.h"
#include "esp_tls.h"
#include "mbedtls/base64.h"

#include "app_settings.h"
#include "app_wifi.h"

static const char *TAG = "mail";

#define SMTP_PORT_TLS       465         /* 唯一支持的端口：隐式 TLS */
#define SMTP_TIMEOUT_MS     15000
#define SMTP_LINE_MAX       512
#define SMTP_BUF_MAX        1024

/*
 * 2023-11-14 的 epoch。系统时间早于它就说明 SNTP 还没对上，此时 TLS
 * 会拿 1970 去验证书有效期，必然失败——报出来的却是个证书错误，看起来
 * 完全是网络问题。所以提前判掉，给一个说得清的原因。
 */
#define TIME_SANE_EPOCH     1700000000L

#define EHLO_NAME           "esp32s3-ecam"

/*---------------------------------------------------------------------------
 * 连接
 *-------------------------------------------------------------------------*/
typedef struct {
    esp_tls_t *tls;
    char       buf[SMTP_LINE_MAX];      /* 读缓冲，可能含多行 */
    size_t     len;
} smtp_conn_t;

/* 往缓冲区补数据。返回 >0 读到的字节数，0 对端关闭，<0 出错 */
static int smtp_fill(smtp_conn_t *c)
{
    if (c->len >= sizeof(c->buf) - 1) {
        ESP_LOGE(TAG, "line too long");
        return -1;
    }
    int n = esp_tls_conn_read(c->tls, c->buf + c->len, sizeof(c->buf) - 1 - c->len);
    if (n > 0) {
        c->len += (size_t)n;
        c->buf[c->len] = '\0';
    }
    return n;
}

/* 读一行，去掉行尾 CR/LF。返回 0 成功，-1 失败 */
static int smtp_read_line(smtp_conn_t *c, char *out, size_t out_sz)
{
    for (;;) {
        char *nl = memchr(c->buf, '\n', c->len);
        if (nl != NULL) {
            size_t line_len = (size_t)(nl - c->buf);
            if (line_len >= out_sz) {
                line_len = out_sz - 1;
            }
            memcpy(out, c->buf, line_len);
            out[line_len] = '\0';
            size_t n = strlen(out);
            while (n > 0 && (out[n - 1] == '\r' || out[n - 1] == '\n')) {
                out[--n] = '\0';
            }
            size_t consumed = (size_t)(nl - c->buf) + 1;
            c->len -= consumed;
            memmove(c->buf, c->buf + consumed, c->len);
            return 0;
        }
        if (smtp_fill(c) <= 0) {
            return -1;
        }
    }
}

/*
 * 读一个完整应答，返回三位状态码，失败返回 -1。
 *
 * SMTP 的多行应答长这样：
 *   250-smtp.example.com
 *   250-SIZE 35882577
 *   250 AUTH LOGIN PLAIN
 * 只有第 4 个字符是空格的那一行才是结束。只读一行的话，剩下的行会留到
 * 下一次读应答时被当成新应答，整个对话从此错位——这是最容易写错的地方。
 */
static int smtp_read_reply(smtp_conn_t *c)
{
    int code = -1;
    /* 多行应答不会有几十行，这个上限只是防对端发疯 */
    for (int i = 0; i < 16; i++) {
        char line[SMTP_LINE_MAX];
        if (smtp_read_line(c, line, sizeof(line)) != 0) {
            ESP_LOGE(TAG, "read reply failed");
            return -1;
        }
        if (strlen(line) < 3) {
            continue;
        }
        code = atoi(line);
        if (line[3] != '-') {
            ESP_LOGD(TAG, "S: %d", code);
            return code;
        }
        ESP_LOGD(TAG, "S: %d-", code);
    }
    return code;
}

static int smtp_write(smtp_conn_t *c, const char *data, size_t len)
{
    size_t sent = 0;
    while (sent < len) {
        int n = esp_tls_conn_write(c->tls, data + sent, len - sent);
        if (n <= 0) {
            ESP_LOGE(TAG, "write failed");
            return -1;
        }
        sent += (size_t)n;
    }
    return 0;
}

/* 发一条命令并检查应答码 */
static bool smtp_expect(smtp_conn_t *c, int want, const char *fmt, ...)
{
    char buf[SMTP_LINE_MAX];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    if (n < 0 || (size_t)n >= sizeof(buf)) {
        return false;
    }
    if (smtp_write(c, buf, (size_t)n) != 0) {
        return false;
    }
    int code = smtp_read_reply(c);
    if (code != want) {
        ESP_LOGE(TAG, "expected %d, got %d", want, code);
        return false;
    }
    return true;
}

static bool b64_encode(const char *in, char *out, size_t out_sz)
{
    size_t olen = 0;
    int rc = mbedtls_base64_encode((unsigned char *)out, out_sz, &olen,
                                   (const unsigned char *)in, strlen(in));
    if (rc != 0) {
        ESP_LOGE(TAG, "base64 failed: %d", rc);
        return false;
    }
    return true;
}

/*---------------------------------------------------------------------------
 * 按域名猜 SMTP 服务器
 *-------------------------------------------------------------------------*/
typedef struct {
    const char *domain;         /* @ 后面那一整段，小写比较 */
    const char *host;           /* NULL = 认识但用不了，见 reason */
    const char *reason;
} smtp_hint_entry_t;

/*
 * 只列"465 隐式 TLS + 密码认证"能用的。微软系单独摘出来放在最后：
 * 它们只支持 587 STARTTLS，而且早就不收密码了（强制 OAuth2，报 5.7.139），
 * 本机两条都占，填了 host 也是白填。
 */
static const smtp_hint_entry_t s_smtp_hints[] = {
    /* --- 国内 --- */
    { "qq.com",         "smtp.qq.com",         NULL },
    { "vip.qq.com",     "smtp.qq.com",         NULL },
    { "foxmail.com",    "smtp.qq.com",         NULL },
    { "163.com",        "smtp.163.com",        NULL },
    { "126.com",        "smtp.126.com",        NULL },
    { "yeah.net",       "smtp.yeah.net",       NULL },
    { "188.com",        "smtp.188.com",        NULL },
    { "139.com",        "smtp.139.com",        NULL },
    { "189.cn",         "smtp.189.cn",         NULL },
    { "sina.com",       "smtp.sina.com",       NULL },
    { "sina.cn",        "smtp.sina.cn",        NULL },
    { "sohu.com",       "smtp.sohu.com",       NULL },
    { "aliyun.com",     "smtp.aliyun.com",     NULL },
    { "21cn.com",       "smtp.21cn.com",       NULL },
    /* --- 国外 --- */
    { "gmail.com",      "smtp.gmail.com",      NULL },
    { "googlemail.com", "smtp.gmail.com",      NULL },
    { "icloud.com",     "smtp.mail.me.com",    NULL },
    { "me.com",         "smtp.mail.me.com",    NULL },
    { "yahoo.com",      "smtp.mail.yahoo.com", NULL },
    { "zoho.com",       "smtp.zoho.com",       NULL },
    /* --- 认识但本机发不了 --- */
    { "outlook.com",    NULL, "Outlook needs OAuth2" },
    { "hotmail.com",    NULL, "Outlook needs OAuth2" },
    { "live.com",       NULL, "Outlook needs OAuth2" },
    { "msn.com",        NULL, "Outlook needs OAuth2" },
    { "office365.com",  NULL, "Office365 needs OAuth2" },
};

const app_mail_smtp_hint_t *app_mail_guess_smtp(const char *email)
{
    static app_mail_smtp_hint_t result;

    if (email == NULL) {
        return NULL;
    }
    /* 用最后一个 @：地址里带引号或注释的写法很少见，不值得为它写解析器 */
    const char *at = strrchr(email, '@');
    if (at == NULL || at[1] == '\0') {
        return NULL;
    }
    const char *domain = at + 1;

    for (size_t i = 0; i < sizeof(s_smtp_hints) / sizeof(s_smtp_hints[0]); i++) {
        /* 比的是 @ 后面那一整段，不是子串匹配，
         * 所以 vip.qq.com 不会被 qq.com 那条顺带命中 */
        if (strcasecmp(domain, s_smtp_hints[i].domain) == 0) {
            result.usable = (s_smtp_hints[i].host != NULL);
            result.host   = s_smtp_hints[i].host;
            result.reason = s_smtp_hints[i].reason;
            return &result;
        }
    }
    return NULL;
}

/*---------------------------------------------------------------------------
 * 对外 API
 *-------------------------------------------------------------------------*/
bool app_mail_is_configured(void)
{
    return app_mail_missing_field() == NULL;
}

const char *app_mail_missing_field(void)
{
    if (app_settings_smtp_host()[0] == '\0') return "SMTP Server";
    if (app_settings_mail_from()[0] == '\0') return "From";
    if (app_settings_mail_pass()[0] == '\0') return "App Password";
    if (app_settings_mail_to()[0]   == '\0') return "To";
    return NULL;
}

const char *app_mail_result_str(app_mail_result_t r)
{
    switch (r) {
    case APP_MAIL_OK:                 return "Email sent";
    case APP_MAIL_ERR_NOT_CONFIGURED: return "Email not set up";
    case APP_MAIL_ERR_NO_WIFI:        return "Connect Wi-Fi first";
    case APP_MAIL_ERR_NO_TIME:        return "Waiting for clock sync";
    case APP_MAIL_ERR_BAD_PORT:       return "Use port 465 (TLS)";
    case APP_MAIL_ERR_CONNECT:        return "Cannot reach server";
    case APP_MAIL_ERR_AUTH:           return "Login refused";
    case APP_MAIL_ERR_REJECTED:       return "Address rejected";
    case APP_MAIL_ERR_SEND:           return "Send failed";
    case APP_MAIL_ERR_NO_MEM:         return "Out of memory";
    case APP_MAIL_ERR_NO_FILE:        return "Cannot read photo";
    case APP_MAIL_ERR_TOO_BIG:        return "Photo too large";
    default:                          return "Send failed";
    }
}

/*---------------------------------------------------------------------------
 * 会话
 *
 * 建连 / 登录 / 信封这三段是测试信和发照片共用的，抽出来免得两处各写
 * 一遍 SMTP 状态机——那种重复一旦有一边改错，表现是"测试信能发、照片
 * 发不出去"这种极难查的问题。
 *-------------------------------------------------------------------------*/
typedef struct {
    smtp_conn_t conn;
    esp_tls_t  *tls;            /* NULL = 没建起来，mail_close 会跳过 */
    const char *host;
    const char *from;
    const char *to;
    char        date[64];       /* RFC 5322 格式，缺了容易被判垃圾邮件 */
    char        domain[64];     /* Message-ID 的域名，取自发件人地址 */
    time_t      sent_at;
} mail_session_t;

/* 未配置 / 没 WiFi / 没对时 / 端口不对。两种发信都得先过这几关 */
static app_mail_result_t mail_preflight(void)
{
    const char *missing = app_mail_missing_field();
    if (missing != NULL) {
        ESP_LOGW(TAG, "not configured, missing: %s", missing);
        return APP_MAIL_ERR_NOT_CONFIGURED;
    }
    if (!app_wifi_is_connected()) {
        return APP_MAIL_ERR_NO_WIFI;
    }
    if (time(NULL) < TIME_SANE_EPOCH) {
        /* 见文件头的说明：不判的话，用户看到的是一个证书错误 */
        ESP_LOGW(TAG, "system clock not synced yet, TLS would reject the cert");
        return APP_MAIL_ERR_NO_TIME;
    }
    if (atoi(app_settings_smtp_port()) != SMTP_PORT_TLS) {
        ESP_LOGW(TAG, "port %s unsupported (only %d, implicit TLS)",
                 app_settings_smtp_port(), SMTP_PORT_TLS);
        return APP_MAIL_ERR_BAD_PORT;
    }
    return APP_MAIL_OK;
}

/*
 * 建 TLS，走完 220 / EHLO / AUTH LOGIN / MAIL FROM / RCPT TO / DATA(354)。
 * 成功时正文通道已打开，调用方接着写 headers + body。
 *
 * 失败时 s->tls 可能仍然非 NULL（比如认证被拒），调用方**必须**调
 * mail_close() 收掉，否则每条失败路径都漏一个 TLS 会话。
 */
static app_mail_result_t mail_open(mail_session_t *s)
{
    app_mail_result_t r = mail_preflight();
    if (r != APP_MAIL_OK) {
        return r;
    }

    s->host = app_settings_smtp_host();
    s->from = app_settings_mail_from();
    s->to   = app_settings_mail_to();
    s->sent_at = time(NULL);

    struct tm tm_local;
    localtime_r(&s->sent_at, &tm_local);
    strftime(s->date, sizeof(s->date), "%a, %d %b %Y %H:%M:%S %z", &tm_local);

    /* Message-ID 的域名用发件人地址里 @ 后面那截：拿一个凭空编的名字
     * 当域名，部分服务端会直接判成垃圾邮件 */
    const char *at = strchr(s->from, '@');
    strlcpy(s->domain, (at != NULL && at[1] != '\0') ? at + 1 : EHLO_NAME,
            sizeof(s->domain));

    esp_tls_cfg_t cfg = {
        .crt_bundle_attach = esp_crt_bundle_attach,
        .timeout_ms        = SMTP_TIMEOUT_MS,
    };
    s->tls = esp_tls_init();
    if (s->tls == NULL) {
        return APP_MAIL_ERR_NO_MEM;
    }

    ESP_LOGI(TAG, "connecting to %s:%d", s->host, SMTP_PORT_TLS);
    if (esp_tls_conn_new_sync(s->host, strlen(s->host), SMTP_PORT_TLS,
                              &cfg, s->tls) != 1) {
        ESP_LOGE(TAG, "TLS connect failed");
        return APP_MAIL_ERR_CONNECT;
    }
    ESP_LOGI(TAG, "TLS established");
    s->conn.tls = s->tls;
    s->conn.len = 0;

    /* 220 是服务器问候，等它先说 */
    if (smtp_read_reply(&s->conn) != 220) {
        return APP_MAIL_ERR_CONNECT;
    }
    if (!smtp_expect(&s->conn, 250, "EHLO %s\r\n", EHLO_NAME)) {
        return APP_MAIL_ERR_CONNECT;
    }

    /*
     * AUTH LOGIN：先发命令，服务器各回一个 334 索要用户名和密码，
     * 两次都发 base64。不要把这些值写进日志。
     */
    char cred[256];
    if (!smtp_expect(&s->conn, 334, "AUTH LOGIN\r\n")) {
        return APP_MAIL_ERR_AUTH;
    }
    if (!b64_encode(s->from, cred, sizeof(cred)) ||
        !smtp_expect(&s->conn, 334, "%s\r\n", cred)) {
        return APP_MAIL_ERR_AUTH;
    }
    if (!b64_encode(app_settings_mail_pass(), cred, sizeof(cred)) ||
        !smtp_expect(&s->conn, 235, "%s\r\n", cred)) {
        return APP_MAIL_ERR_AUTH;
    }
    ESP_LOGI(TAG, "authenticated");

    if (!smtp_expect(&s->conn, 250, "MAIL FROM:<%s>\r\n", s->from)) {
        return APP_MAIL_ERR_REJECTED;
    }
    if (!smtp_expect(&s->conn, 250, "RCPT TO:<%s>\r\n", s->to)) {
        return APP_MAIL_ERR_REJECTED;
    }
    if (!smtp_expect(&s->conn, 354, "DATA\r\n")) {
        return APP_MAIL_ERR_SEND;
    }
    return APP_MAIL_OK;
}

static void mail_close(mail_session_t *s, bool ok)
{
    if (s->tls == NULL) {
        return;
    }
    if (ok) {
        /* QUIT 的应答丢了也无所谓，信已经收了 */
        smtp_expect(&s->conn, 221, "QUIT\r\n");
    }
    esp_tls_conn_destroy(s->tls);
    s->tls = NULL;
    s->conn.tls = NULL;
}

/*---------------------------------------------------------------------------
 * 正文：纯文本测试信
 *-------------------------------------------------------------------------*/
app_mail_result_t app_mail_send_test(void)
{
    mail_session_t s = { 0 };
    app_mail_result_t r = mail_open(&s);
    if (r != APP_MAIL_OK) {
        mail_close(&s, false);
        return r;
    }

    char body[SMTP_BUF_MAX];
    int n = snprintf(body, sizeof(body),
                     "From: %s\r\n"
                     "To: %s\r\n"
                     "Subject: ESP32S3_Ecam test email\r\n"
                     "Date: %s\r\n"
                     "Message-ID: <ecam-%ld@%s>\r\n"
                     "MIME-Version: 1.0\r\n"
                     "Content-Type: text/plain; charset=UTF-8\r\n"
                     "\r\n"
                     "This is a test message from the ESP32S3_Ecam camera.\r\n"
                     "If you can read this, the mail settings are working.\r\n"
                     "\r\n"          /* 正文结束的空行 */
                     ".\r\n",        /* 单独一行的句点 = 正文结束 */
                     s.from, s.to, s.date, (long)s.sent_at, s.domain);
    if (n < 0 || (size_t)n >= sizeof(body) ||
        smtp_write(&s.conn, body, (size_t)n) != 0 ||
        smtp_read_reply(&s.conn) != 250) {
        r = APP_MAIL_ERR_SEND;
        mail_close(&s, false);
        return r;
    }

    mail_close(&s, true);
    ESP_LOGI(TAG, "test email accepted by server");
    return APP_MAIL_OK;
}

/*---------------------------------------------------------------------------
 * 正文：JPEG 附件
 *-------------------------------------------------------------------------*/
#define MIME_BOUNDARY   "----ecam-boundary"
/* 每次读 3072 字节。必须是 3 的倍数：这样每块能独立 base64 编码后直接
 * 拼起来，不会在块边界出现补位的 '='。 */
#define B64_RAW_CHUNK   (3 * 1024)
#define B64_OUT_CHUNK   ((B64_RAW_CHUNK / 3) * 4)   /* 4096 */
#define B64_LINE_LEN    76                          /* RFC 2045 规定 */

/*
 * 把文件分块 base64 后写进正文。整文件读进内存的话，8MB 的附件会变成
 * ~11MB 的 base64 再翻一倍缓冲，没必要。
 */
static app_mail_result_t mime_write_base64(mail_session_t *s, const char *path)
{
    FILE *f = fopen(path, "rb");
    if (f == NULL) {
        ESP_LOGW(TAG, "cannot open attachment: %s", path);
        return APP_MAIL_ERR_NO_FILE;
    }

    uint8_t *raw = malloc(B64_RAW_CHUNK);
    char    *b64 = malloc(B64_OUT_CHUNK + 8);
    if (raw == NULL || b64 == NULL) {
        free(raw);
        free(b64);
        fclose(f);
        return APP_MAIL_ERR_NO_MEM;
    }

    app_mail_result_t r = APP_MAIL_OK;
    bool first_line = true;     /* 头部末尾的空行已经把正文带起来了，
                                 * 第一行 base64 前面不能再加 CRLF */

    for (;;) {
        size_t got = fread(raw, 1, B64_RAW_CHUNK, f);
        if (got == 0) {
            break;
        }
        size_t olen = 0;
        if (mbedtls_base64_encode((unsigned char *)b64, B64_OUT_CHUNK + 8,
                                  &olen, raw, got) != 0) {
            r = APP_MAIL_ERR_SEND;
            break;
        }
        for (size_t off = 0; off < olen; ) {
            size_t line = olen - off;
            if (line > B64_LINE_LEN) {
                line = B64_LINE_LEN;
            }
            if (!first_line && smtp_write(&s->conn, "\r\n", 2) != 0) {
                r = APP_MAIL_ERR_SEND;
                goto out;
            }
            if (smtp_write(&s->conn, b64 + off, line) != 0) {
                r = APP_MAIL_ERR_SEND;
                goto out;
            }
            first_line = false;
            off += line;
        }
    }
    if (ferror(f)) {
        r = APP_MAIL_ERR_NO_FILE;
    }

out:
    free(raw);
    free(b64);
    fclose(f);
    return r;
}

/* 附件文件名：取路径最后一段 */
static const char *base_name(const char *path)
{
    const char *slash = strrchr(path, '/');
    return (slash != NULL) ? slash + 1 : path;
}

app_mail_result_t app_mail_send_photos(const char *const *paths, int count)
{
    return app_mail_send_files(paths, count, "image/jpeg", "photo");
}

app_mail_result_t app_mail_send_files(const char *const *paths, int count,
                                      const char *mime, const char *kind)
{
    if (paths == NULL || count <= 0) {
        return APP_MAIL_ERR_NO_FILE;
    }
    if (count > APP_MAIL_MAX_ATTACHMENTS) {
        ESP_LOGW(TAG, "too many attachments (%d), capped at %d",
                 count, APP_MAIL_MAX_ATTACHMENTS);
        count = APP_MAIL_MAX_ATTACHMENTS;
    }

    /*
     * 先逐个 stat，把"文件在不在"和"总共多大"都在建连之前查清楚。
     * 不然连上服务器、认证完、正文都发了一半才发现某个附件读不了，
     * 只能半路断线，用户看到的是一个没头没尾的失败。
     */
    long total = 0;
    for (int i = 0; i < count; i++) {
        struct stat st;
        if (paths[i] == NULL || stat(paths[i], &st) != 0 || st.st_size <= 0) {
            ESP_LOGW(TAG, "cannot stat attachment: %s",
                     paths[i] ? paths[i] : "(null)");
            return APP_MAIL_ERR_NO_FILE;
        }
        total += (long)st.st_size;
    }
    if (total > APP_MAIL_MAX_ATTACH) {
        ESP_LOGW(TAG, "attachments too big: %ld bytes total", total);
        return APP_MAIL_ERR_TOO_BIG;
    }

    mail_session_t s = { 0 };
    app_mail_result_t r = mail_open(&s);
    if (r != APP_MAIL_OK) {
        mail_close(&s, false);
        return r;
    }

    /* 头部 + 文本段。第一段的分隔线紧跟在头部的空行之后，前面不加 CRLF；
     * 后面的每一段都以 CRLF 开头（见下面的循环）。 */
    char hdr[SMTP_BUF_MAX];
    int n = snprintf(hdr, sizeof(hdr),
                     "From: %s\r\n"
                     "To: %s\r\n"
                     "Subject: ESP32S3_Ecam %s%s\r\n"
                     "Date: %s\r\n"
                     "Message-ID: <ecam-%ld@%s>\r\n"
                     "MIME-Version: 1.0\r\n"
                     "Content-Type: multipart/mixed; boundary=\"%s\"\r\n"
                     "\r\n"
                     "--%s\r\n"
                     "Content-Type: text/plain; charset=UTF-8\r\n"
                     "\r\n"
                     "%d %s(s) sent from the ESP32S3_Ecam, attached.\r\n",
                     s.from, s.to, kind, (count > 1) ? "s" : "",
                     s.date, (long)s.sent_at, s.domain,
                     MIME_BOUNDARY, MIME_BOUNDARY, count, kind);
    if (n < 0 || (size_t)n >= sizeof(hdr) ||
        smtp_write(&s.conn, hdr, (size_t)n) != 0) {
        r = APP_MAIL_ERR_SEND;
        goto done;
    }

    for (int i = 0; i < count; i++) {
        const char *fname = base_name(paths[i]);
        char part[SMTP_BUF_MAX];

        /* 开头的 CRLF 结束文本段（base64 写出来的最后一行不带换行） */
        n = snprintf(part, sizeof(part),
                     "\r\n--%s\r\n"
                     "Content-Type: %s; name=\"%s\"\r\n"
                     "Content-Transfer-Encoding: base64\r\n"
                     "Content-Disposition: attachment; filename=\"%s\"\r\n"
                     "\r\n",
                     MIME_BOUNDARY, mime, fname, fname);
        if (n < 0 || (size_t)n >= sizeof(part) ||
            smtp_write(&s.conn, part, (size_t)n) != 0) {
            r = APP_MAIL_ERR_SEND;
            goto done;
        }

        r = mime_write_base64(&s, paths[i]);
        if (r != APP_MAIL_OK) {
            goto done;
        }
        ESP_LOGI(TAG, "attached %s (%d/%d)", fname, i + 1, count);
    }

    /* 收尾：关边界 → 正文结束的句点 */
    char tail[64];
    n = snprintf(tail, sizeof(tail), "\r\n--%s--\r\n.\r\n", MIME_BOUNDARY);
    if (n < 0 || (size_t)n >= sizeof(tail) ||
        smtp_write(&s.conn, tail, (size_t)n) != 0 ||
        smtp_read_reply(&s.conn) != 250) {
        r = APP_MAIL_ERR_SEND;
        goto done;
    }

    ESP_LOGI(TAG, "sent %d photo(s), %ld bytes total", count, total);
    r = APP_MAIL_OK;

done:
    mail_close(&s, r == APP_MAIL_OK);
    return r;
}

app_mail_result_t app_mail_send_photo(const char *path)
{
    return app_mail_send_photos(&path, 1);
}
