/*
 * motion.h — 履带车的差速电机控制
 *
 * 两路 N20 电机，每路两个 LEDC 通道（一个正转、一个反转，见 motion.c 的说明）。
 */
#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* 建 LEDC 定时器和四个通道。重复调用是空操作 */
esp_err_t motion_init(void);

/*
 * 差速控制。x 是转向、y 是前后，各自 -1..1（超出会被夹住）。
 * **y 正数 = 前进**，和 Ecam 上摇杆"往屏幕上方推"一致。
 */
void motion_drive(float x, float y);

/* 两路都停。看门狗超时和上电都用它 */
void motion_stop(void);

#ifdef __cplusplus
}
#endif
