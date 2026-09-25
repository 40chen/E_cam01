/*
 * app_tank.c — 履带车模块的 Ecam 这一侧
 *
 * 一个任务跑两件事：每 50ms 发一条运动指令（顺带当心跳），并收 C2 回的话。
 * 协议见 app_tank.h。
 *
 * **为什么发送节拍不能交给界面**：摇杆回调每秒能出上百个事件，一个事件发
 * 一条串口就是拿事件率当波特率用。界面只写两个 float，发多少、什么时候发
 * 归这里决定——和 app_audio 那条"音量不走命令队列"是同一个道理，方向相反。
 *
 * 丢链时会**自己先把向量归零**（不是等 C2 那 500ms 看门狗）：失控的车是
 * 安全问题，宁可多发几帧停车指令，也不要赌对面还醒着。
 */
#include "app_tank.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include "driver/uart.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "lwip/inet.h"
#include "lwip/sockets.h"

#include "app_module.h"
#include "app_settings.h"
#include "app_wifi.h"

static const char *TAG = "tank";

#define TANK_UART        UART_NUM_1
#define TANK_TX_PIN      APP_MODULE_GPIO_A      /* GPIO10 */
#define TANK_RX_PIN      APP_MODULE_GPIO_B      /* GPIO11 */
#define TANK_BAUD        115200
#define TANK_TICK_MS     50                     /* 20Hz，和参考项目一致 */
#define TANK_LINE_MAX    128
#define TANK_RX_BUF      1024

/* 凭据事务：三行一起发，等结果；超时就重发整批（整批是幂等的） */
#define TANK_WIFI_RETRY      3
#define TANK_WIFI_TIMEOUT_MS 1000

static TaskHandle_t      s_task;
static volatile bool     s_stop;
static SemaphoreHandle_t s_lock;

static volatile float s_vx, s_vy;               /* 摇杆向量，界面写、任务读 */
static int64_t        s_last_rx_us;             /* 0 = 还没收到过任何字节 */
static app_tank_status_t s_status;

/*
 * UDP 那条无线驱动链路。
 *
 * **sock 由控制任务独占**（建、收、发、关都在它里面），所以不用加锁；
 * IP 这一份要跟界面共享，走 s_lock（并同时镜像到 s_status.ip 里给界面读）。
 */
static int  s_udp_sock = -1;
static char s_c2_ip[16];                        /* 空 = 还不知道，那就走 UART */
static struct sockaddr_in s_c2_addr;
static bool s_udp_ready;

/* 凭据事务的状态。**只有控制任务碰它们**，界面那边只置一个 request 标志 */
static volatile bool s_wifi_request;
static bool     s_wifi_pending;
static int      s_wifi_tries;
static int64_t  s_wifi_sent_us;

/*---------------------------------------------------------------------------
 * 小工具
 *-------------------------------------------------------------------------*/
/* 标准 base64。凭据里有空格、有中文 SSID 都是常事，而协议是按空白分列的，
 * 所以载荷必须编码——base64 是编码不是加密，见 README 的说明 */
static size_t b64_encode(const uint8_t *in, size_t len, char *out, size_t out_size)
{
    static const char tbl[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    size_t o = 0;
    for (size_t i = 0; i < len && o + 4 < out_size; i += 3) {
        const uint32_t v = (uint32_t)in[i] << 16 |
                           (uint32_t)(i + 1 < len ? in[i + 1] : 0) << 8 |
                           (uint32_t)(i + 2 < len ? in[i + 2] : 0);
        out[o++] = tbl[(v >> 18) & 0x3F];
        out[o++] = tbl[(v >> 12) & 0x3F];
        out[o++] = (i + 1 < len) ? tbl[(v >> 6) & 0x3F] : '=';
        out[o++] = (i + 2 < len) ? tbl[v & 0x3F] : '=';
    }
    out[o] = '\0';
    return o;
}

/*---------------------------------------------------------------------------
 * 收行
 *-------------------------------------------------------------------------*/
static void mark_alive(void)
{
    s_last_rx_us = esp_timer_get_time();
}

/*
 * 一条来自车的行。**UART 和 UDP 两条路都走这里**——协议是同一套，只是
 * 换了传输，所以不需要第二套解析。
 */
static void handle_line(const char *line)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (strncmp(line, "#ok ", 4) == 0) {
        /* C2 报的身份：能握上手才推凭据（不答 = 旧固件，推过去它也不认识） */
        strlcpy(s_status.version, line + 4, sizeof(s_status.version));
        ESP_LOGI(TAG, "模块应答: %s", s_status.version);
    } else if (strncmp(line, "#ip ", 4) == 0) {
        strlcpy(s_status.ip, line + 4, sizeof(s_status.ip));
        /* 学到地址 = 从此可以走无线。车每秒还会再报一次当心跳，
         * 所以这里不用区分"第一次"和"又来了" */
        const bool first = (s_c2_ip[0] == '\0');
        strlcpy(s_c2_ip, line + 4, sizeof(s_c2_ip));
        if (first && s_c2_ip[0] != '\0') {
            ESP_LOGI(TAG, "拿到车的地址 %s，之后的驱动走 UDP:%d",
                     s_c2_ip, APP_TANK_UDP_PORT);
        }
    } else if (strncmp(line, "#wifi ok", 8) == 0) {
        s_wifi_pending = false;
        s_status.wifi_sent = true;
        ESP_LOGI(TAG, "凭据已送达");
    } else if (strncmp(line, "#wifi err", 9) == 0) {
        s_wifi_pending = false;
        ESP_LOGW(TAG, "模块拒绝凭据:%s", line + 9);
    } else {
        /* 我们自己的运动指令被回显回来，或者别的噪音。**不解析**，只当
         * "链接还活着"——回显的内容本来就是我们自己刚写的 */
        ESP_LOGD(TAG, "忽略: %s", line);
    }
    xSemaphoreGive(s_lock);
}

/*---------------------------------------------------------------------------
 * 凭据事务
 *-------------------------------------------------------------------------*/
static void wifi_send_batch(void)
{
    char b64[128];
    char line[192];

    /* SSID / PASS 的长度上限是 app_settings 定的（32 / 64），
     * base64 之后最大 88 字符，装得下 */
    b64_encode((const uint8_t *)app_settings_wifi_ssid(),
               strlen(app_settings_wifi_ssid()), b64, sizeof(b64));
    int n = snprintf(line, sizeof(line), "#wifi 1 %s\n", b64);
    uart_write_bytes(TANK_UART, line, n);

    b64_encode((const uint8_t *)app_settings_wifi_pass(),
               strlen(app_settings_wifi_pass()), b64, sizeof(b64));
    n = snprintf(line, sizeof(line), "#wifi 2 %s\n", b64);
    uart_write_bytes(TANK_UART, line, n);

    /* 第三行是"提交"。C2 到这一行才写 NVS——前两行只进临时缓冲，
     * 半途断了不会在 C2 上留下半套凭据 */
    uart_write_bytes(TANK_UART, "#wifi 3\n", 8);
}

static void wifi_begin(void)
{
    ESP_LOGI(TAG, "推送 WiFi 凭据（SSID %u 字节）",
             (unsigned)strlen(app_settings_wifi_ssid()));
    s_wifi_pending  = true;
    s_wifi_tries    = 0;
    s_wifi_sent_us  = esp_timer_get_time();
    wifi_send_batch();
}

static void wifi_tick(int64_t now)
{
    if (!s_wifi_pending) {
        return;
    }
    if ((now - s_wifi_sent_us) / 1000 < TANK_WIFI_TIMEOUT_MS) {
        return;
    }
    if (s_wifi_tries >= TANK_WIFI_RETRY) {
        s_wifi_pending = false;
        ESP_LOGW(TAG, "凭据没送达（重试 %d 次），车那头可能还是旧固件", TANK_WIFI_RETRY);
        return;
    }
    s_wifi_tries++;
    s_wifi_sent_us = now;
    ESP_LOGW(TAG, "凭据无应答，重发（第 %d 次）", s_wifi_tries);
    wifi_send_batch();
}

/*---------------------------------------------------------------------------
 * UDP：无线那条驱动链路
 *-------------------------------------------------------------------------*/
/*
 * 建 socket 并绑到我们自己的端口。**绑定是必要的**：不绑的话车回的
 * `#ip` 心跳会送到一个随机口上，我们收不到，链接状态就永远是断的。
 *
 * 只在控制任务里调（socket 归它独占）。
 */
static void udp_open(void)
{
    if (s_udp_sock >= 0) {
        return;
    }
    s_udp_sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s_udp_sock < 0) {
        ESP_LOGE(TAG, "UDP socket 建不起来: errno %d", errno);
        return;
    }

    struct sockaddr_in me = {
        .sin_family = AF_INET,
        .sin_addr.s_addr = htonl(INADDR_ANY),
        .sin_port = htons(APP_TANK_UDP_PORT),
    };
    if (bind(s_udp_sock, (struct sockaddr *)&me, sizeof(me)) < 0) {
        ESP_LOGE(TAG, "UDP bind %d 失败: errno %d", APP_TANK_UDP_PORT, errno);
        close(s_udp_sock);
        s_udp_sock = -1;
        return;
    }
    s_udp_ready = true;
    ESP_LOGI(TAG, "UDP 已就绪，本机口 %d", APP_TANK_UDP_PORT);
}

static void udp_close(void)
{
    if (s_udp_sock >= 0) {
        close(s_udp_sock);
        s_udp_sock = -1;
    }
    s_udp_ready = false;
    s_c2_addr.sin_addr.s_addr = 0;
    /*
     * **车的 IP 故意不清**。
     *
     * 它的来路只有一条：插着线的时候车用 UART 报上来。而这一版的设计正是
     * "配完网就把线拔了，车跑开"——要是停一次模块就把地址丢掉，用户想临时
     * 换个模块再换回来，就得把车抓回来重新插线，而车可能正在屋子另一头。
     *
     * 留着它最坏也就是"车换了 IP 之后往老地址发空包"，无害，而且页面上
     * 那个地址是看得见的，用户能自己判断。重新插线自然就刷新了。
     * （掉电还是会丢，这个没做持久化——那要给 app_settings 加键，
     * 等真觉得每次开机都要插一次线很烦再说。）
     */
}

/*
 * 发一包摇杆数据。返回 true = 发出去了（不代表车收到了——UDP 没有这个保证，
 * 车的存活靠它自己回的 `#ip` 心跳来判断）。
 */
static bool udp_send_vector(const char *line, int len)
{
    if (!s_udp_ready || s_c2_ip[0] == '\0' || !app_wifi_is_connected()) {
        return false;
    }
    if (s_c2_addr.sin_addr.s_addr == 0) {
        /* 地址是字符串进、socket 地址是整数出，只在这里转一次 */
        s_c2_addr.sin_family = AF_INET;
        s_c2_addr.sin_port = htons(APP_TANK_UDP_PORT);
        s_c2_addr.sin_addr.s_addr = inet_addr(s_c2_ip);
        if (s_c2_addr.sin_addr.s_addr == INADDR_NONE) {
            ESP_LOGW(TAG, "车的地址 '%s' 不合法，退回 UART", s_c2_ip);
            return false;
        }
    }
    /* 非阻塞发送：这里在 20Hz 的任务里，卡住的话整条链路都跟着卡 */
    const int n = sendto(s_udp_sock, line, len, MSG_DONTWAIT,
                         (struct sockaddr *)&s_c2_addr, sizeof(s_c2_addr));
    if (n < 0) {
        ESP_LOGD(TAG, "UDP 发送失败: errno %d", errno);
        return false;
    }
    return true;
}

/*
 * 把攒着的包都收干净，一行一行喂给同一个 handle_line。
 * 非阻塞：没包就立刻返回。
 */
static void udp_drain(void)
{
    if (!s_udp_ready) {
        return;
    }
    char    line[TANK_LINE_MAX];
    size_t  len = 0;
    bool    over = false;

    for (int guard = 0; guard < 8; guard++) {       /* 一轮最多处理 8 个包 */
        char buf[256];
        struct sockaddr_in from;
        socklen_t fromlen = sizeof(from);
        const int n = recvfrom(s_udp_sock, buf, sizeof(buf), MSG_DONTWAIT,
                               (struct sockaddr *)&from, &fromlen);
        if (n <= 0) {
            return;
        }
        mark_alive();

        /* 车回话的源地址就是我们记的那个 IP，顺手对齐一次——万一它换了地址
         * 而这条心跳还能到我们这儿（比如 DHCP 续约给了同一个），就不用插线 */
        char ip[16];
        inet_ntoa_r(from.sin_addr, ip, sizeof(ip));
        xSemaphoreTake(s_lock, portMAX_DELAY);
        if (strcmp(s_c2_ip, ip) != 0) {
            ESP_LOGI(TAG, "车的地址从 %s 变成 %s", s_c2_ip, ip);
            strlcpy(s_c2_ip, ip, sizeof(s_c2_ip));
            s_c2_addr.sin_addr.s_addr = 0;          /* 下次发送时重算 */
        }
        xSemaphoreGive(s_lock);

        for (int i = 0; i < n; i++) {
            const char c = buf[i];
            if (c == '\n') {
                if (!over && len > 0) {
                    line[len] = '\0';
                    handle_line(line);
                }
                len = 0;
                over = false;
                continue;
            }
            if (c == '\r' || over) {
                continue;
            }
            if (len >= sizeof(line) - 1) {
                over = true;
                continue;
            }
            line[len++] = c;
        }
    }
}

/*---------------------------------------------------------------------------
 * 控制任务
 *-------------------------------------------------------------------------*/
static void tank_task(void *arg)
{
    (void)arg;

    char    line[TANK_LINE_MAX];
    size_t  len = 0;
    bool    over = false;
    uint8_t buf[128];

    while (!s_stop) {
        const int64_t now = esp_timer_get_time();

        /* ---- 1. 发运动指令。丢链时发的是 0,0 ---- */
        const bool up = (s_last_rx_us != 0) &&
                        ((now - s_last_rx_us) / 1000 < APP_TANK_LINK_TIMEOUT_MS);
        float x = s_vx, y = s_vy;
        if (!up) {
            x = 0.0f;
            y = 0.0f;
        }
        char out[40];
        const int n = snprintf(out, sizeof(out), "x%.2f y%.2f\n", x, y);

        /*
         * **知道车的 IP 就走无线，否则走那根线。**
         * 两条路不会同时发：同时发的话车上会收到两份一样的指令（无害，
         * 但没意义），而且没法从日志判断到底走的哪条。
         *
         * 注意"有线兜底"不是只在没配网时有用：IP 记错了、车换了地址还没
         * 重新插线的时候，线一连上就能继续开。
         */
        if (!udp_send_vector(out, n)) {
            uart_write_bytes(TANK_UART, out, n);
        }

        /* ---- 2. 收 ---- */
        const int got = uart_read_bytes(TANK_UART, buf, sizeof(buf), 0);
        for (int i = 0; i < got; i++) {
            const char c = (char)buf[i];
            mark_alive();
            if (c == '\n') {
                if (!over) {
                    line[len] = '\0';
                    if (len > 0) {
                        handle_line(line);
                    }
                }
                len = 0;
                over = false;
                continue;
            }
            if (c == '\r' || over) {
                continue;
            }
            if (len >= sizeof(line) - 1) {
                /* 超长整行丢掉，不拼接。和副屏那边同一条原则：
                 * 宁可丢一帧也不要拼半行，半行解析出来的东西没法信 */
                over = true;
                ESP_LOGW(TAG, "收到超长行，丢弃");
                continue;
            }
            line[len++] = c;
        }

        /* ---- 2b. 收 UDP（无线那条路的回话在这儿进来） ---- */
        udp_drain();

        /* ---- 3. 链接状态 ---- */
        xSemaphoreTake(s_lock, portMAX_DELAY);
        const bool was_up = s_status.up;
        s_status.up = up;
        xSemaphoreGive(s_lock);
        if (was_up != up) {
            ESP_LOGW(TAG, "链接 %s", up ? "恢复" : "断开（没有收到任何字节）");
        }

        /* ---- 4. 凭据事务 ---- */
        if (s_wifi_request) {
            s_wifi_request = false;
            wifi_begin();
        }
        wifi_tick(now);

        vTaskDelay(pdMS_TO_TICKS(TANK_TICK_MS));
    }

    s_task = NULL;
    vTaskDelete(NULL);
}

/*---------------------------------------------------------------------------
 * 对外接口
 *-------------------------------------------------------------------------*/
esp_err_t app_tank_enable(void)
{
    if (app_module_selected() != APP_MODULE_TANK) {
        return ESP_ERR_INVALID_STATE;
    }
    if (s_task != NULL) {
        return ESP_OK;
    }
    if (s_lock == NULL) {
        s_lock = xSemaphoreCreateMutex();
        if (s_lock == NULL) {
            return ESP_ERR_NO_MEM;
        }
    }

    /* 顺序照 app_pcmon.c：装驱动 → 配参数 → 设引脚。
     * 用的是旧版 driver/uart.h——IDF 5.1 只有这一套（esp_driver_uart 的
     * 拆分是 5.3 的事），而且工程里已有先例可抄 */
    esp_err_t err = uart_driver_install(TANK_UART, TANK_RX_BUF, 0, 0, NULL, 0);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "uart driver install 失败: %s", esp_err_to_name(err));
        return err;
    }

    const uart_config_t cfg = {
        .baud_rate  = TANK_BAUD,
        .data_bits  = UART_DATA_8_BITS,
        .parity     = UART_PARITY_DISABLE,
        .stop_bits  = UART_STOP_BITS_1,
        .flow_ctrl  = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };
    uart_param_config(TANK_UART, &cfg);
    uart_set_pin(TANK_UART, TANK_TX_PIN, TANK_RX_PIN,
                 UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
    uart_flush_input(TANK_UART);

    /* UDP 那条无线链路。**建不起来不是致命的**：还能走 UART，
     * 而且 IP 本来就要靠 UART 才能学到 */
    udp_open();

    /* 清状态：刚选上模块时一切从头开始，不要沿用上一次连接的数字 */
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_last_rx_us = 0;
    s_wifi_pending = false;
    s_wifi_request = false;
    memset(&s_status, 0, sizeof(s_status));
    xSemaphoreGive(s_lock);
    s_vx = s_vy = 0.0f;

    s_stop = false;
    if (xTaskCreatePinnedToCore(tank_task, "tank", 3072, NULL, 4, &s_task, 1) != pdPASS) {
        s_task = NULL;
        uart_driver_delete(TANK_UART);
        app_module_port_idle();
        return ESP_ERR_NO_MEM;
    }

    /* 问一次身份。答不答都不影响控制，只决定"能不能推凭据" */
    uart_write_bytes(TANK_UART, "#hi\n", 4);

    ESP_LOGI(TAG, "履带车链路已开（UART%d TX=%d RX=%d @%d）",
             TANK_UART, TANK_TX_PIN, TANK_RX_PIN, TANK_BAUD);
    return ESP_OK;
}

void app_tank_disable(void)
{
    if (s_task == NULL) {
        return;
    }
    s_stop = true;
    for (int i = 0; i < 25 && s_task != NULL; i++) {
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    if (s_task != NULL) {
        ESP_LOGE(TAG, "控制任务没退出来，UART 保持原样");
        return;
    }

    /*
     * **补发停车指令再拆驱动**：任务一停就没人发了，而车要等 500ms 看门狗
     * 才自己停——这半秒车是照着最后一条向量跑的。
     *
     * **两条路都发**：此刻不知道用户刚才走的是哪条，而且很可能线早就拔了、
     * 车挂在无线那头。车收到两次 0,0 没有任何坏处，漏发一次却可能让它多跑
     * 半秒，所以不挑。
     */
    for (int i = 0; i < 3; i++) {
        udp_send_vector("x0.00 y0.00\n", 12);
        uart_write_bytes(TANK_UART, "x0.00 y0.00\n", 12);
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    vTaskDelay(pdMS_TO_TICKS(50));      /* 让最后一条真的出得去 */

    uart_driver_delete(TANK_UART);
    udp_close();
    app_module_port_idle();
    ESP_LOGI(TAG, "履带车链路已关（UART 与 UDP 都收了）");
}

bool app_tank_is_active(void)
{
    return s_task != NULL;
}

void app_tank_set_vector(float x, float y)
{
    if (x < -1.0f) x = -1.0f;
    if (x > 1.0f)  x = 1.0f;
    if (y < -1.0f) y = -1.0f;
    if (y > 1.0f)  y = 1.0f;
    s_vx = x;
    s_vy = y;
}

esp_err_t app_tank_send_wifi(void)
{
    if (s_task == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    if (!app_settings_has_wifi()) {
        return ESP_ERR_INVALID_STATE;
    }
    /* 只置标志，真正的三行和重试都在控制任务里做——界面回调不该等串口 */
    s_wifi_request = true;
    return ESP_OK;
}

void app_tank_get_status(app_tank_status_t *out)
{
    if (out == NULL || s_lock == NULL) {
        return;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    *out = s_status;
    xSemaphoreGive(s_lock);
}
