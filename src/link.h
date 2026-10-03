/*
 * link.h — 和 Ecam 的 UART 链路
 *
 * 协议见 Ecam 仓库 README 的"外接模块 → 履带车"一节（两边必须一致）：
 *
 *   Ecam → C2   x<f> y<f>\n          差速控制，x 转向、y 前后，各 -1..1
 *   Ecam → C2   #hi                  问身份
 *   C2 → Ecam   #ok v1 tank          应答
 *   Ecam → C2   #wifi 1 <b64 SSID>
 *   Ecam → C2   #wifi 2 <b64 PASS>
 *   Ecam → C2   #wifi 3              到这一行才真的存 NVS 并连
 *   C2 → Ecam   #wifi ok | #wifi err <code>
 *   C2 → Ecam   #ip <addr>           拿到 IP / 掉线（空地址）；收到 #hi 时也报告当前状态
 *
 * **运动指令 500ms 不来就自动停车**（和参考项目的看门狗同长）。
 */
#pragma once

#include <stdbool.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* 建 UART + 起解析任务。重复调用是空操作 */
esp_err_t link_start(void);

/* 主动往 Ecam 报一次 IP（wifi_prov 的回调会调它） */
void link_send_ip(const char *ip);

/*
 * 处理一条命令。**UART 和 UDP 两条路共用这一个解析器**（协议是同一套，
 * 只是换了传输）。
 *
 * allow_provisioning：只有 UART 那条路能传 true。
 * **UDP 上必须传 false**——不然局域网里任何人往这个端口打一行 `#wifi 1 ...`
 * 就能改掉车上的 WiFi 凭据。运动指令无所谓（反正网页遥控也是无鉴权的，
 * 用户已经接受了这一点），但凭据不能让外人写。
 */
void link_handle_line(char *line, bool allow_provisioning);

#ifdef __cplusplus
}
#endif
