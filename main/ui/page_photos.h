/*
 * page_photos.h — 相册页（TF 卡里的照片）
 *
 * 两视图同页：网格（3 列缩略图）→ 点击进大图。大图视图盖在 lv_layer_top()
 * 上，因为页面外壳的根对象是 flex 纵向布局，往里塞全屏子对象会被 flex
 * 重新摆位，压不住标题栏。
 *
 * 缩略图不走后台任务：用 lv_timer 每拍解码一张，全程在 LVGL 上下文里。
 * 解码一张几十毫秒，代价是加载期间界面略卡；换来的是彻底不用处理
 * "解码任务比页面活得久"那类竞态（use-after-free、晚到的回调操作已销毁
 * 的控件）。相册是静态视图，卡一点可以接受。
 */
#pragma once

#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

lv_obj_t *page_photos_create(lv_obj_t *parent);
void      page_photos_destroy(void);

#ifdef __cplusplus
}
#endif
