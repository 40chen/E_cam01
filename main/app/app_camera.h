/*
 * app_camera.h — 摄像头服务
 *
 * 负责：初始化 GC0308、以 ~30fps 取帧并回调给界面、按需抓拍并软件编码成 JPEG。
 *
 * 本模块刻意不依赖 LVGL：取到的 RGB565 帧通过回调交给 UI 层，
 * 由 UI 决定怎么显示（当前做法是零拷贝挂到 lv_img 上）。
 *
 * 水平仪叠加层已按需求移除，交出去的帧就是原始画面。
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

/* 帧回调。rgb565 指向摄像头内部缓冲区，仅在回调期间有效，
 * 不要保存指针或在回调返回后访问。 */
typedef void (*app_camera_frame_cb_t)(const uint8_t *rgb565, uint32_t width,
                                      uint32_t height, void *user);

/* 初始化摄像头并启动取帧任务。失败返回错误，界面应显示"相机不可用"。 */
esp_err_t app_camera_start(void);

/* 停止取帧并释放摄像头（U 盘模式、进入休眠前调用） */
esp_err_t app_camera_stop(void);

bool app_camera_is_running(void);

/* 注册/注销取帧回调。传 NULL 表示暂停取帧（页面切走后必须注销，
 * 否则回调会操作已经被销毁的 LVGL 对象）。 */
void app_camera_set_frame_cb(app_camera_frame_cb_t cb, void *user);

/* 抓拍一帧 -> 软件 JPEG 编码 -> 写入 TF 卡。
 * out_path 非空时回填实际保存的完整路径。 */
esp_err_t app_camera_capture(char *out_path, size_t path_len);
