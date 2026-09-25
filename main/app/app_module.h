/*
 * app_module.h — 模块口：外接功能模块的启用与停止
 *
 * 模块口是板上仅剩的两根空脚（GPIO10/11）。**型号由人来识别**：用户在
 * Modules 页上点选，固件按选择把那两根脚交给对应驱动。所以这里
 * **没有任何检测**——不读引脚、不用电阻分压、不用 ADC，模块侧也不需要放
 * 识别电阻。"插了什么"这个问题问人，不问脚。
 *
 * 代价写在明处：**插拔模块之前要先把当前模块关掉**（或者至少知道会怎样）。
 * 一根脚同一时刻只能是一个外设的。
 *
 * 一次只跑一个模块是**软件规则**，不是电气事实——界面不给同时选两个，
 * 服务层再用 app_module_selected() 让每个驱动的 enable() 自我校验一次。
 */
#pragma once

#include <stdbool.h>

#include "driver/gpio.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* 模块口的两根脚。A 是 WS2812 的数据线 / 履带车的 TX，B 是履带车的 RX */
#define APP_MODULE_GPIO_A   (GPIO_NUM_10)
#define APP_MODULE_GPIO_B   (GPIO_NUM_11)

typedef enum {
    APP_MODULE_NONE = 0,
    APP_MODULE_LED2812,     /* WS2812 8x8 氛围灯，DIN = GPIO10 */
    APP_MODULE_TANK,        /* 履带车，UART1 重映射到 GPIO10/11 */
    APP_MODULE_PRINTER,     /* 打印机：还没做，列表里只显示不可点 */
    APP_MODULE_COUNT,
} app_module_id_t;

/*
 * 开机调一次：把两根脚停到"空闲态"（输入 + 内部上拉）。
 * 无任务、不分配内存，失败也不该拦住启动。
 */
esp_err_t app_module_init(void);

/*
 * 选一个模块：先停旧的，再启动新的。失败时回到 NONE 并返回错误，
 * **绝不停在"旧的已停、新的没起来"的中间态**——那会让模块口谁都不管。
 * 已经选着同一个时是空操作（界面重复点不会重启一次硬件）。
 * 未实现的型号返回 ESP_ERR_NOT_SUPPORTED。
 */
esp_err_t app_module_select(app_module_id_t id);

/* 停掉当前模块，回到空闲态 */
void app_module_deselect(void);

app_module_id_t app_module_selected(void);

/*
 * 把 GPIO10/11 还给空闲态。**每个驱动的 deinit 末尾必须调它**：
 * 无论是 RMT 还是 UART 驱动都不会自己还脚，不还的话 GPIO10 就停在一根
 * 被推挽驱动的输出线上——下次插上的是别的模块，那根线就是被硬顶着。
 *
 * 注意这是**引脚卫生，不是检测**：它只看自己刚用过的脚，不推断插了什么。
 */
void app_module_port_idle(void);

/* 列表里显示的名字（"WS2812 Matrix" 这种） */
const char *app_module_name(app_module_id_t id);

/* 这个型号做了没有。没做的在列表里置灰 */
bool app_module_implemented(app_module_id_t id);

#ifdef __cplusplus
}
#endif
