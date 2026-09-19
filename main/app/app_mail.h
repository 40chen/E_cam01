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
 * 一封邮件带多个附件（multipart/mixed），相册页发照片用。
 * 等价于 app_mail_send_files(paths, count, "image/jpeg", "photo")。
 */
app_mail_result_t app_mail_send_photos(const char *const *paths, int count);

/*
 * 同上，但附件类型和邮件文案由调用方给：mime 是每个附件段的 Content-Type
 * （"image/jpeg" / "audio/mpeg"），kind 用来拼主题和正文（"photo" / "music"）。
 * 音乐页发 MP3 用这个——把 .mp3 标成 image/jpeg 虽然多数邮件客户端会自己
 * 嗅探内容，但那是碰运气，不该指望。
 *
 * 同样**阻塞**，要在自己的任务里调；逐个附件流式编码，不会把文件读进内存。
 * paths 只在调用期间用，返回后调用方可以释放。
 * 超过 APP_MAIL_MAX_ATTACHMENTS 会被截断（只记警告，不报错），
 * 总大小超过 APP_MAIL_MAX_ATTACH 直接返回 APP_MAIL_ERR_TOO_BIG。
 */
app_mail_result_t app_mail_send_files(const char *const *paths, int count,
                                      const char *mime, const char *kind);

/*
 * 单封邮件的附件总量上限。**18MB 是算出来的，不是拍的**：
 *
 *   - 收件方（Gmail 等）普遍限制**整封邮件** 25MB，而附件 base64 之后要膨胀
 *     4/3：18MB 的附件编码完约 24MB，加上头正好压在 25MB 以内。再往上加会
 *     被对方服务器拒收，而且是**整封传完之后**才拒，白等几分钟。
 *   - 这么大在 TLS 下大约要传 1~2 分钟（WiFi 上实测 100~300KB/s）。这期间
 *     界面显示 Sending...，属正常，不是卡死；SMTP_TIMEOUT_MS 是单次读写的
 *     超时，不会把整封传一半掐掉。
 *   - 不吃 RAM：附件是分块读、分块 base64 的，不会把文件读进内存。所以调大
 *     这个值只吃时间，不冒险。
 */
#define APP_MAIL_MAX_ATTACH         (18 * 1024 * 1024)
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
