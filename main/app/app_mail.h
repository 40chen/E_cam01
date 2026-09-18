/*
 * app_mail.h — SMTP 发信
 *
 * 目前只有"发一封测试邮件"这一个动作，作用是让设置页里那几栏邮箱配置
 * 有个地方能验证：存进去到底能不能用。相册页的发照片是后续的事。
 *
 * 传输用 esp-tls + 系统证书包，只支持**隐式 TLS（465 端口）**。
 * 587 的 STARTTLS 需要先明文握手再升级加密，esp-tls 没有提供这条路，
 * 所以端口不是 465 就直接报错，不装作能连。
 */
#pragma once

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    APP_MAIL_OK = 0,
    APP_MAIL_ERR_NOT_CONFIGURED,    /* 服务器 / 发件人 / 密码 / 收件人有空 */
    APP_MAIL_ERR_NO_WIFI,
    APP_MAIL_ERR_NO_TIME,           /* 系统时间还没同步，TLS 没法校验证书 */
    APP_MAIL_ERR_BAD_PORT,          /* 端口不是 465 */
    APP_MAIL_ERR_CONNECT,           /* TCP / TLS 建连失败 */
    APP_MAIL_ERR_AUTH,              /* 服务器拒绝账号密码 */
    APP_MAIL_ERR_REJECTED,          /* 发件人或收件人被拒 */
    APP_MAIL_ERR_SEND,              /* 正文阶段出错 */
    APP_MAIL_ERR_NO_MEM,
    APP_MAIL_ERR_NO_FILE,           /* 附件打不开 / 是空文件 */
    APP_MAIL_ERR_TOO_BIG,           /* 附件超出上限 */
} app_mail_result_t;

/* 四项必填（服务器 / 发件人 / 密码 / 收件人）是否都非空 */
bool app_mail_is_configured(void);

/* 未配置时，返回第一个空着的那一项的名字（如 "SMTP Server"），用于提示 */
const char *app_mail_missing_field(void);

/*
 * 发一封测试邮件。**阻塞**，整个 TLS 握手 + SMTP 对话大约几秒，
 * 必须在自己的任务里调，不要占用 LVGL 任务或主任务。
 */
app_mail_result_t app_mail_send_test(void);

/*
 * 把 path 指向的 JPEG 作为附件发出去（multipart/mixed）。
 * 同样**阻塞**，要在自己的任务里调。附件是分块读、分块 base64 编码的，
 * 不会把整个文件读进内存。
 * 超过 APP_MAIL_MAX_ATTACH 直接返回 APP_MAIL_ERR_TOO_BIG。
 */
app_mail_result_t app_mail_send_photo(const char *path);

/*
 * 一封邮件带多个附件（multipart/mixed，每张一个 image/jpeg 段）。
 * 同样**阻塞**，要在自己的任务里调。逐个附件流式编码，不会把文件读进内存。
 * paths 只在调用期间用，返回后调用方可以释放。
 * 超过 APP_MAIL_MAX_ATTACHMENTS 会被截断（只记警告，不报错）。
 */
app_mail_result_t app_mail_send_photos(const char *const *paths, int count);

/* 单封邮件的附件上限：单个文件，以及所有附件加起来 */
#define APP_MAIL_MAX_ATTACH         (8 * 1024 * 1024)
#define APP_MAIL_MAX_ATTACHMENTS    20

/* 短文案，直接进 UI 提示条 */
const char *app_mail_result_str(app_mail_result_t r);

/*---------------------------------------------------------------------------
 * 按邮箱域名猜 SMTP 服务器
 *-------------------------------------------------------------------------*/
typedef struct {
    bool        usable;     /* 认出来了，而且本机能发 */
    const char *host;       /* usable 时的 SMTP 服务器，否则 NULL */
    const char *reason;     /* usable 为 false 时给用户看的一句话 */
} app_mail_smtp_hint_t;

/*
 * 用发件人邮箱 @ 后面那一整段去查表。认不出来返回 NULL。
 *
 * 返回 usable == false 的情况：域名是常见的，但服务商不支持"465 隐式 TLS
 * + 密码认证"这套组合（微软系只给 587 STARTTLS 且强制 OAuth2）。
 * 这种情况**不要**去填 host——填一个连不上的地址比不填更糟，用户只会看到
 * 一个没头没尾的连接失败。把 reason 显示出来就行。
 */
const app_mail_smtp_hint_t *app_mail_guess_smtp(const char *email);

#ifdef __cplusplus
}
#endif
