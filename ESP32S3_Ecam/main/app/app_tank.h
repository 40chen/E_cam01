/*
 * app_tank.h — 履带车模块（外接模块之一）
 *
 * ## 两条链路，分工是清楚的
 *
 * **UART（模块口 GPIO10/11）只干一件事：把 WiFi 凭据交给车。**
 * 车连上 WiFi、把 IP 报回来之后，**这根线就可以拔掉了**——之后的驱动走
 * 无线。所以"插着线才能开"的时代结束了，车是可以跑开的。
 *
 * **UDP 是真正的驱动链路**：Ecam 往车报上来的 IP 单播 20Hz 的摇杆数据。
 * 两边在同一个 WiFi 下，车被拎到屋子另一头也照样开。
 *
 *   UART:  Ecam GPIO10 (TX) → C2 RXD    /  Ecam GPIO11 (RX) ← C2 TXD
 *   UDP :  Ecam ──20Hz 摇杆包──→ C2:APP_TANK_UDP_PORT
 *          Ecam ←─1Hz #ip 心跳─ C2
 *
 * ## 协议是同一套纯文本行，只是跑在两个传输上
 *
 *   Ecam → C2   x<f> y<f>\n         差速控制：x 转向、y 前后，各 -1..1
 *   Ecam → C2   #hi                 问身份
 *   C2 → Ecam   #ok v1 tank         应答（不答 = 旧固件，不推凭据）
 *   Ecam → C2   #wifi 1 <b64 SSID>  凭据事务，三行连着发（只走 UART）
 *   Ecam → C2   #wifi 2 <b64 PASS>
 *   Ecam → C2   #wifi 3             ← C2 到这一行才真的写 NVS
 *   C2 → Ecam   #wifi ok | #wifi err <code>
 *   C2 → Ecam   #ip <addr>          C2 拿到 IP 或收到 #hi 时报告；之后每秒一条当心跳
 *
 * 两边**共用同一个行解析器**（handle_line），UDP 那条路上收到的也是这些行，
 * 不需要第二套协议。
 *
 * **`#` 开头是刻意的**：没升级的 C2 撞上不认识的命令会走
 * `sscanf("%c%f")` 的 Unknown 分支，动都不动——不认识的行必须无害，
 * 否则"给新模块推凭据"这件事会变成"让旧模块乱跑"。
 *
 * ## 发哪条路，怎么选
 *
 * **知道车的 IP 就走 UDP，否则走 UART。** IP 只有 UART 能告诉它（`#ip`），
 * 而 UDP 是单播，所以**车重启后如果拿到了新 IP，得再插一次线**——这是选单播
 * 换来的代价，README 里写清楚了；页面上显示的那个地址就是当前在用的它。
 *
 * 20Hz 的摇杆数据**居中时也照发**：它同时是心跳，而且安全值就是停车值。
 * 丢链就把向量归零，让车停住（车那边自己还有 500ms 看门狗兜底）。
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * 车那边约定的 UDP 端口（收发都在这个口上）。两边必须一致。
 * 选 3333 只是"别撞 80/443/DHCP/DNS 那些"，没有别的含义。
 */
#define APP_TANK_UDP_PORT  3333

/*
 * 超过这么久没收到车的任何回话就算丢链，界面上显示 No response。
 *
 * **1500 而不是 500**：车的心跳是 1Hz（`#ip`，顺便让 Ecam 刷新它记的地址），
 * 500ms 会把正常心跳也判成丢链，界面一直闪红灯。
 *
 * 这个数**只影响界面显示**，不影响车的安全：真正让车停下来的是车上那个
 * 500ms 看门狗（它只看有没有新的驱动指令，不看这个）。两边故意不是一个数：
 * 一个是"用户看得对不对"，一个是"车该不该停"。
 */
#define APP_TANK_LINK_TIMEOUT_MS  1500

typedef struct {
    bool up;                    /* 链接是否活着 */
    bool wifi_sent;             /* 凭据是否已经成功送过去 */
    char version[16];           /* C2 报的版本，空 = 没握上手 */
    char ip[16];                /* C2 报的 IP，空 = 还不知道 */
} app_tank_status_t;

/*
 * 起 UART1 + 控制任务。要求已经 app_module_select(APP_MODULE_TANK)，
 * 否则返回 ESP_ERR_INVALID_STATE。
 */
esp_err_t app_tank_enable(void);

/*
 * 停链接：停任务 → **补发停车指令** → 拆 UART → 把脚还给空闲态。
 * 中间那步是安全相关，别省：任务一停就没人再发指令了，而 C2 那边只会
 * 在 500ms 后自己停——这半秒车是照最后一条向量跑的。
 * IDF 也不会自己还脚（uart_driver_delete 不动引脚配置）。
 */
void app_tank_disable(void);

bool app_tank_is_active(void);

/*
 * 摇杆向量，x/y 各 -1..1。**界面在事件回调里写，发送节拍归服务任务**——
 * 手指划过屏幕每秒能出上百个事件，让事件率直接变成串口流量是错的。
 */
void app_tank_set_vector(float x, float y);

/*
 * 把 Ecam 的 WiFi 凭据推给 C2。非阻塞：真正的重试在控制任务的 tick 里做。
 * 没有配过 WiFi 返回 ESP_ERR_INVALID_STATE（界面该提示先去设置页填）。
 */
esp_err_t app_tank_send_wifi(void);

void app_tank_get_status(app_tank_status_t *out);

#ifdef __cplusplus
}
#endif
