/*
 * ui.h — 屏幕管理
 *
 * 导航模型：图标主菜单（开机页）+ 全屏子页。
 * 同一时刻只有一个页面对象存在，切换即销毁旧页、创建新页。
 *
 * 线程约定：
 *  - ui_goto() / ui_go_home() 可以在任意任务调用，内部会加 LVGL 锁。
 *  - 在 LVGL 事件回调里切页必须用 ui_goto_async()：回调执行期间直接
 *    销毁控件（尤其是触发事件的那个控件的父链）在 LVGL 里是不安全的。
 */
#pragma once

#include "esp_err.h"
#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    UI_PAGE_HOME = 0,
    UI_PAGE_CAMERA,
    UI_PAGE_PHOTOS,
    UI_PAGE_MUSIC,
    UI_PAGE_SETTINGS,
    UI_PAGE_WORKMODE,       /* 工作模式：会改变整机形态的开关 */
    UI_PAGE_MODULES,        /* 外接模块：选模块的地方 */
    UI_PAGE_MOD_LED,        /* 模块控制页：WS2812 氛围灯 */
    UI_PAGE_MOD_TANK,       /* 模块控制页：履带车 */
    UI_PAGE_PCMON,          /* 副屏：上位机推过来的系统状态 */
    UI_PAGE_COUNT,
} ui_page_id_t;

/* 初始化 LVGL 端口与触摸，先显示 ERDCX 启动页，随后自动进入主菜单 */
esp_err_t ui_init(void);

/* 立即切换页面（任意任务可调） */
void ui_goto(ui_page_id_t page);

/* 就地推迟到下一次 LVGL 处理时切页（事件回调里用这个） */
void ui_goto_async(ui_page_id_t page);

/* 回到主菜单 */
void ui_go_home(void);
void ui_go_home_async(void);

/* 当前页面 id */
ui_page_id_t ui_current_page(void);

/*---------------------------------------------------------------------------
 * 页面模块实现。ui.c 通过下表调用，每个页面都是"create 返回外壳、destroy
 * 负责善后"这一对。
 *-------------------------------------------------------------------------*/
lv_obj_t *page_home_create(lv_obj_t *parent);
void      page_home_destroy(void);

lv_obj_t *page_camera_create(lv_obj_t *parent);
void      page_camera_destroy(void);

lv_obj_t *page_photos_create(lv_obj_t *parent);
void      page_photos_destroy(void);

lv_obj_t *page_music_create(lv_obj_t *parent);
void      page_music_destroy(void);

lv_obj_t *page_settings_create(lv_obj_t *parent);
void      page_settings_destroy(void);

lv_obj_t *page_workmode_create(lv_obj_t *parent);
void      page_workmode_destroy(void);

lv_obj_t *page_modules_create(lv_obj_t *parent);
void      page_modules_destroy(void);

lv_obj_t *page_mod_led_create(lv_obj_t *parent);
void      page_mod_led_destroy(void);

lv_obj_t *page_mod_tank_create(lv_obj_t *parent);
void      page_mod_tank_destroy(void);

lv_obj_t *page_pcmon_create(lv_obj_t *parent);
void      page_pcmon_destroy(void);

/* 主菜单按钮触发的动作，供 page_home.c 使用 */
void ui_open_photos(void);
void ui_open_music(void);
void ui_open_settings(void);
void ui_open_workmode(void);
void ui_open_modules(void);
void ui_open_mod_led(void);
void ui_open_mod_tank(void);
void ui_open_pcmon(void);

#ifdef __cplusplus
}
#endif
