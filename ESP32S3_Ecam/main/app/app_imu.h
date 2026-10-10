/*
 * app_imu.h — 板载姿态传感器 QMI8658（用来判断设备是不是倒过来了）
 *
 * 只做一件事：**看重力方向，决定画面要不要翻 180°**。
 * 判方向用加速度计，不用陀螺仪——陀螺仪是角速度，积分出来会漂，
 * "哪边是上"本来就是加速度计的活（QMI8658 两颗都有）。
 *
 * I2C：和 PCA9557 / 触摸 / ES8311 / 相机 SCCB 共用 I2C0（GPIO1/2，400kHz），
 * 从机地址 **0x6a**（原理图上 SA0 接地；如果读不到会自动再试 0x6b，
 * 见 app_imu.c 的自检）。
 *
 * **第一步是自检，不是直接上功能**：WHO_AM_I（期望 0x05）不通过就什么都不做，
 * 只把扫描结果打进串口。这样"读不到数据"这类问题能一眼看到底是哪一层
 * （地址不对 / 芯片没焊 / 总线被占），而不是屏幕上"转了但没反应"。
 */
#pragma once

#include <stdbool.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * 自检 + 配置 + 起采样任务。**失败不致命**（板上没有这颗芯片也该能开机）：
 * 返回错误时 app_imu_ok() 为 false，翻转功能自动失效，别的功能一切照旧。
 */
esp_err_t app_imu_init(void);

/* 自检通过且采样任务在跑 */
bool app_imu_ok(void);

/*
 * 设备是不是倒着的（需要把画面翻 180°）。关掉开关时恒为 false。
 * 判定带迟滞，见 app_imu.c。
 */
bool app_imu_upside_down(void);

/* 自动翻转的开关。**掉电不保留**（设置页那边另有持久化，见 app_settings） */
void app_imu_set_enabled(bool on);
bool app_imu_enabled(void);

/*
 * 一行状态，给设置页那一行显示用。**这是给"串口日志看不见"留的后路**：
 * 传感器在不在、开关开没开、当前判成什么，一眼就能看出来，不用去翻串口。
 *
 *   "No sensor"     自检没过（0x6a / 0x6b 都没应答），功能是关的
 *   "Off"           传感器在，开关关着
 *   "On"            传感器在、开着、当前判成正立
 *   "Upside down"   传感器在、开着、当前判成倒置
 *
 * 返回的是静态字符串，随时可读（不用加锁）
 */
const char *app_imu_status_str(void);

#ifdef __cplusplus
}
#endif
