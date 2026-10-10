/*
 * link.c — 和 Ecam 的 UART 协议
 *
 * **行式解析，不是"读一段就 sscanf"**：参考项目是每 20ms 读一次缓冲区然后
 * 直接 sscanf，凭据那种一百多字节的行一定会被切成两半。这里改成攒到 '\n'
 * 为止再解析，超过上限就整行丢掉（宁可丢一帧也不要拼半行）。
 *
 * **不认识的行必须无害**：Ecam 那边发来的 `#` 开头的东西，新固件认识就处理，
 * 不认识就只打条日志、什么都不做。这条是双向兼容的地基——不然给新模块推
 * 凭据会变成让旧模块乱跑。
 */
#include "link.h"
#include "esp_check.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "driver/uart.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "motion.h"
#include "wifi_prov.h"

static const char *TAG = "link";

/* 串口脚。**这是 C2 自己的脚号**，和 Ecam 那边是两套编号：
 * Ecam 的 TX(GPIO10) 接这里的 RX，Ecam 的 RX(GPIO11) 接这里的 TX。
 * 沿用参考项目 c2_tracked_chassis 的值（TX=10 / RX=18）。 */
#define LINK_UART       UART_NUM_1
#define LINK_TX_PIN     (10)
#define LINK_RX_PIN     (18)
#define LINK_BAUD       115200

#define LINK_LINE_MAX   160         /* 凭据行最长约 100 字节，留足余量 */
#define LINK_TICK_MS    20          /* 解析节拍，和参考项目一致 */

static bool     s_started;
static char     s_cred_ssid[66];    /* base64 解码后的临时缓冲，等 #wifi 3 提交 */
static char     s_cred_pass[96];
static bool     s_cred_pending;

/*---------------------------------------------------------------------------
 * base64 解码（Ecam 那边编的，见 app_tank.c 的 b64_encode）
 *-------------------------------------------------------------------------*/
static int b64_val(char c)
{
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+') return 62;
    if (c == '/') return 63;
    return -1;
}

/* 返回写出的字节数，-1 表示格式不对。out 保证以 '\0' 结尾 */
static int b64_decode(const char *in, char *out, size_t out_size)
{
    size_t o = 0;
    int quad[4];
    int n = 0;

    for (const char *p = in; *p != '\0'; p++) {
        if (*p == '=') {
            break;
        }
        const int v = b64_val(*p);
        if (v < 0) {
            return -1;              /* 非法字符：整条命令不认 */
        }
        quad[n++] = v;
        if (n == 4) {
            const uint32_t w = (uint32_t)((quad[0] << 18) | (quad[1] << 12) |
                                          (quad[2] << 6) | quad[3]);
            if (o + 3 >= out_size) {
                return -1;          /* 解出来太长 */
            }
            out[o++] = (char)((w >> 16) & 0xFF);
            out[o++] = (char)((w >> 8) & 0xFF);
            out[o++] = (char)(w & 0xFF);
            n = 0;
        }
    }
    /* 收尾：剩 2 或 3 个字符（1 个字符是不合法的） */
    if (n == 2) {
        const uint32_t w = (uint32_t)((quad[0] << 18) | (quad[1] << 12));
        if (o + 1 >= out_size) {
            return -1;
        }
        out[o++] = (char)((w >> 16) & 0xFF);
    } else if (n == 3) {
        const uint32_t w = (uint32_t)((quad[0] << 18) | (quad[1] << 12) | (quad[2] << 6));
        if (o + 2 >= out_size) {
            return -1;
        }
        out[o++] = (char)((w >> 16) & 0xFF);
        out[o++] = (char)((w >> 8) & 0xFF);
    } else if (n != 0) {
        return -1;
    }
    out[o] = '\0';
    return (int)o;
}

/*---------------------------------------------------------------------------
 * 回话
 *-------------------------------------------------------------------------*/
static void reply(const char *s)
{
    uart_write_bytes(LINK_UART, s, strlen(s));
}

void link_send_ip(const char *ip)
{
    if (!s_started) {
        return;
    }
    char buf[32];
    const int n = snprintf(buf, sizeof(buf), "#ip %s\n", ip != NULL ? ip : "");
    uart_write_bytes(LINK_UART, buf, n);
}

/*---------------------------------------------------------------------------
 * 命令
 *-------------------------------------------------------------------------*/
void link_handle_line(char *line, bool allow_provisioning)
{
    /* ---- 运动指令：x<f> y<f> ---- */
    if (line[0] == 'x') {
        float x = 0.0f, y = 0.0f;
        if (sscanf(line, "x%f y%f", &x, &y) == 2) {
            motion_drive(x, y);     /* 喂看门狗在 motion_drive 里，两条路共用 */
            return;
        }
        ESP_LOGW(TAG, "运动指令格式不对: %s", line);
        return;
    }

    if (line[0] != '#') {
        ESP_LOGD(TAG, "忽略: %s", line);
        return;
    }

    /* ---- 身份 ---- */
    if (strcmp(line, "#hi") == 0) {
        reply("#ok v1 tank\n");
        /* Ecam 可能在 WiFi 已经连好后才启用履带车模块；
         * #hi 握手时重报当前 IP，避免错过启动时唯一一次 IP 事件。 */
        link_send_ip(wifi_prov_connected() ? wifi_prov_ip() : "");
        return;
    }

    /* ---- 凭据事务 ---- */
    if (strncmp(line, "#wifi ", 6) == 0) {
        if (!allow_provisioning) {
            /* 从无线来的凭据一律不认。见 link.h 里的说明：这条不是防君子，
             * 是"局域网里任何人打一行就能改掉车的配网"确实太好用了 */
            ESP_LOGW(TAG, "拒绝来自无线的凭据写入: %s", line);
            reply("#wifi err 4\n");
            return;
        }
        const char *body = line + 6;
        if (body[0] == '1' && body[1] == ' ') {
            if (b64_decode(body + 2, s_cred_ssid, sizeof(s_cred_ssid)) < 0) {
                reply("#wifi err 1\n");
                return;
            }
            s_cred_pending = true;
            ESP_LOGI(TAG, "收到 SSID（%u 字节），等提交", (unsigned)strlen(s_cred_ssid));
            return;
        }
        if (body[0] == '2' && body[1] == ' ') {
            if (b64_decode(body + 2, s_cred_pass, sizeof(s_cred_pass)) < 0) {
                reply("#wifi err 1\n");
                return;
            }
            ESP_LOGI(TAG, "收到密码，等提交");
            return;
        }
        if (strcmp(body, "3") == 0) {
            /* **到这一行才真的落盘并连**：前两行只进临时缓冲，
             * 传到一半断了不会在车上留下半套凭据 */
            if (!s_cred_pending) {
                reply("#wifi err 3\n");     /* 没收到 SSID 就提交 */
                return;
            }
            s_cred_pending = false;
            if (wifi_prov_set(s_cred_ssid, s_cred_pass) != ESP_OK) {
                reply("#wifi err 2\n");
                return;
            }
            /* 这里只回"已收下"，连上与否走 #ip / 连不上没有回报——
             * Ecam 那边判的是"凭据送达"，不是"网络通了" */
            reply("#wifi ok\n");
            return;
        }
        reply("#wifi err 2\n");         /* 不认识的子命令 */
        return;
    }

    ESP_LOGI(TAG, "不认识的命令（忽略）: %s", line);
}

/*---------------------------------------------------------------------------
 * 任务
 *-------------------------------------------------------------------------*/
static void link_task(void *arg)
{
    (void)arg;

    char   line[LINK_LINE_MAX];
    size_t len = 0;
    bool   over = false;
    uint8_t buf[128];

    while (1) {
        const int got = uart_read_bytes(LINK_UART, buf, sizeof(buf),
                                        pdMS_TO_TICKS(LINK_TICK_MS));

        for (int i = 0; i < got; i++) {
            const char c = (char)buf[i];
            if (c == '\n') {
                if (!over && len > 0) {
                    line[len] = '\0';
                    /* 有线这条路才能推凭据（见 link_handle_line 的说明） */
                    link_handle_line(line, true);
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
                ESP_LOGW(TAG, "超长行，丢弃");
                continue;
            }
            line[len++] = c;
        }
        /* 停车看门狗不在这儿——它归 motion.c 管，因为网页那条路也能驱动车，
         * 只看 UART 会把手机遥控的车每 500ms 停一次 */
    }
}

/*---------------------------------------------------------------------------
 * 对外接口
 *-------------------------------------------------------------------------*/
esp_err_t link_start(void)
{
    if (s_started) {
        return ESP_OK;
    }

    const uart_config_t cfg = {
        .baud_rate  = LINK_BAUD,
        .data_bits  = UART_DATA_8_BITS,
        .parity     = UART_PARITY_DISABLE,
        .stop_bits  = UART_STOP_BITS_1,
        .flow_ctrl  = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };
    ESP_RETURN_ON_ERROR(uart_driver_install(LINK_UART, 1024, 1024, 0, NULL, 0),
                        TAG, "uart install");
    ESP_RETURN_ON_ERROR(uart_param_config(LINK_UART, &cfg), TAG, "uart config");
    ESP_RETURN_ON_ERROR(uart_set_pin(LINK_UART, LINK_TX_PIN, LINK_RX_PIN,
                                     UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE),
                        TAG, "uart pins");
    uart_flush_input(LINK_UART);

    if (xTaskCreate(link_task, "link", 4096, NULL, 5, NULL) != pdPASS) {
        uart_driver_delete(LINK_UART);
        return ESP_ERR_NO_MEM;
    }

    s_started = true;
    ESP_LOGI(TAG, "链路已开（UART%d TX=%d RX=%d @%d）",
             LINK_UART, LINK_TX_PIN, LINK_RX_PIN, LINK_BAUD);
    return ESP_OK;
}
