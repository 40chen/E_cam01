/*
 * udp.c — 无线驱动链路
 *
 * 一个任务，两件事：收 Ecam 的摇杆包（喂给 motion 和那个共用的行解析器），
 * 以及每秒回一条 `#ip` 心跳。
 *
 * **心跳是必要的，不是礼貌**：Ecam 那边靠"有没有收到车的话"来判断链接，
 * 而 UDP 的驱动流是单向的——不发心跳的话，页面上的状态永远是"没响应"，
 * 用户会以为车掉线了。
 *
 * 安全相关的一点：**从无线来的 `#wifi` 一律拒绝**（见 link.h）。摇杆指令
 * 无所谓——网页遥控本来就是无鉴权的，用户接受过这一点；但凭据不能让局域网
 * 里随便谁写。
 */
#include "udp.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/inet.h"
#include "lwip/sockets.h"

#include "link.h"
#include "motion.h"
#include "wifi_prov.h"

static const char *TAG = "udp";

#define UDP_RECV_TIMEOUT_MS 200
#define UDP_HEARTBEAT_MS    1000
#define UDP_LINE_MAX        128

static int s_sock = -1;

/* 最近一次收到包的来源（就是 Ecam）。回心跳用它 */
static struct sockaddr_in s_peer;
static bool               s_have_peer;

static int64_t s_last_hb_us;

static void send_heartbeat(void)
{
    if (!s_have_peer) {
        return;
    }
    char line[32];
    /* 复用 `#ip` 这条命令当心跳：信息就是"我在，我的地址是这个"，
     * Ecam 那边已经会解析它（它本来是用来报地址的），不用新增协议 */
    const int n = snprintf(line, sizeof(line), "#ip %s\n",
                           wifi_prov_ip()[0] ? wifi_prov_ip() : "?");
    sendto(s_sock, line, n, 0, (struct sockaddr *)&s_peer, sizeof(s_peer));
    s_last_hb_us = esp_timer_get_time();
}

static void udp_task(void *arg)
{
    (void)arg;

    char line[UDP_LINE_MAX];

    while (1) {
        char buf[256];
        struct sockaddr_in from;
        socklen_t fromlen = sizeof(from);

        const int n = recvfrom(s_sock, buf, sizeof(buf), 0,
                               (struct sockaddr *)&from, &fromlen);
        if (n > 0) {
            /* 记下 Ecam 的地址。它可能换 IP（路由器重新分配），所以每包都更新，
             * 而不是只记第一次 */
            if (!s_have_peer || from.sin_addr.s_addr != s_peer.sin_addr.s_addr) {
                s_peer = from;
                s_have_peer = true;
                s_last_hb_us = 0;   /* 让下面立刻回一条，Ecam 那边好尽快亮灯 */
                char ip[16];
                inet_ntoa_r(from.sin_addr, ip, sizeof(ip));
                ESP_LOGI(TAG, "Ecam 在 %s", ip);
            }

            /* 一个包里可能是整行，也可能没有换行（UDP 有报文边界，
             * Ecam 发的是带 '\n' 的整行）。两种都按行处理 */
            int start = 0;
            for (int i = 0; i <= n; i++) {
                if (i == n || buf[i] == '\n') {
                    const int len = i - start;
                    if (len > 0 && len < (int)sizeof(line)) {
                        memcpy(line, buf + start, len);
                        line[len] = '\0';
                        /* false：无线上不许写凭据 */
                        link_handle_line(line, false);
                    }
                    start = i + 1;
                }
            }
        }

        if (s_have_peer &&
            (esp_timer_get_time() - s_last_hb_us) / 1000 >= UDP_HEARTBEAT_MS) {
            send_heartbeat();
        }
    }
}

esp_err_t udp_start(void)
{
    if (s_sock >= 0) {
        return ESP_OK;
    }

    s_sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s_sock < 0) {
        ESP_LOGE(TAG, "socket 建不起来: errno %d", errno);
        return ESP_FAIL;
    }

    const struct sockaddr_in me = {
        .sin_family = AF_INET,
        .sin_addr.s_addr = htonl(INADDR_ANY),
        .sin_port = htons(UDP_PORT),
    };
    if (bind(s_sock, (const struct sockaddr *)&me, sizeof(me)) < 0) {
        ESP_LOGE(TAG, "bind %d 失败: errno %d", UDP_PORT, errno);
        close(s_sock);
        s_sock = -1;
        return ESP_FAIL;
    }

    /* 收包超时：没包的时候要能醒来发心跳 */
    const struct timeval tv = { .tv_sec = 0, .tv_usec = UDP_RECV_TIMEOUT_MS * 1000 };
    setsockopt(s_sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    if (xTaskCreate(udp_task, "udp", 4096, NULL, 5, NULL) != pdPASS) {
        close(s_sock);
        s_sock = -1;
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG, "无线驱动已就绪（UDP %d），等 Ecam 来包", UDP_PORT);
    return ESP_OK;
}
