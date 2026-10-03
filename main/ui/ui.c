#include "ui.h"

#include <stdlib.h>

#include "esp_log.h"
#include "esp_lvgl_port.h"
#include "lckfb_bsp.h"

#include "app_imu.h"
#include "splash_logo.h"
#include "ui_theme.h"

static const char *TAG = "ui";

typedef struct {
    lv_obj_t *(*create)(lv_obj_t *parent);
    void (*destroy)(void);
} page_desc_t;

static lv_obj_t *s_current = NULL;
static void (*s_current_destroy)(void) = NULL;
static ui_page_id_t s_current_id = UI_PAGE_COUNT;

static void switch_page_locked(ui_page_id_t page);

/* 自动翻转（QMI8658 判方向）。全局一个定时器，不随页面走，见 ui_init */
static lv_timer_t *s_rotate_timer;

static void rotate_timer_cb(lv_timer_t *t)
{
    (void)t;
    /* 面板的 MADCTL 和触摸的镜像位都要改（bsp_lvgl_set_flip 里一起做了）。
     * 放在 lv_timer 里是为了跟着 LVGL 的节拍走——翻转靠的是"让 LVGL 整屏重画
     * 一遍"，而这一拍恰好就在 lv_timer_handler 里。锁由 bsp_lvgl_set_flip 自己
     * 取（递归锁），所以这里不需要再包一层。 */
    bsp_lvgl_set_flip(app_imu_upside_down());
}

static const page_desc_t s_pages[UI_PAGE_COUNT] = {
    [UI_PAGE_HOME]     = { page_home_create,     page_home_destroy },
    [UI_PAGE_CAMERA]   = { page_camera_create,   page_camera_destroy },
    [UI_PAGE_PHOTOS]   = { page_photos_create,   page_photos_destroy },
    [UI_PAGE_MUSIC]    = { page_music_create,    page_music_destroy },
    [UI_PAGE_SETTINGS] = { page_settings_create, page_settings_destroy },
    [UI_PAGE_WORKMODE] = { page_workmode_create, page_workmode_destroy },
    [UI_PAGE_MODULES]  = { page_modules_create,  page_modules_destroy  },
    [UI_PAGE_MOD_LED]  = { page_mod_led_create,  page_mod_led_destroy  },
    [UI_PAGE_MOD_TANK] = { page_mod_tank_create, page_mod_tank_destroy },
    [UI_PAGE_PCMON]    = { page_pcmon_create,    page_pcmon_destroy    },
};

/*---------------------------------------------------------------------------
 * 开机启动页：黑底白字居中显示品牌名。
 * 面板上电后 GRAM 里是随机内容，背光一亮就是花屏；BSP 已经先把整屏刷黑，
 * 这里再把这一屏变成有意义的画面，等主菜单准备好再切过去。
 *-------------------------------------------------------------------------*/
#define SPLASH_HOLD_MS 1200

static lv_obj_t   *s_splash = NULL;
static lv_timer_t *s_splash_timer = NULL;

static void splash_dismiss_locked(void)
{
    /* 定时器触发时 LVGL 会自己回收它，回调里已经把指针置空了 */
    if (s_splash_timer != NULL) {
        lv_timer_del(s_splash_timer);
        s_splash_timer = NULL;
    }
    if (s_splash != NULL) {
        lv_obj_del(s_splash);
        s_splash = NULL;
    }
}

static void splash_timer_cb(lv_timer_t *t)
{
    (void)t;
    s_splash_timer = NULL;
    splash_dismiss_locked();
    switch_page_locked(UI_PAGE_HOME);
}

static void splash_show_locked(void)
{
    s_splash = lv_obj_create(lv_scr_act());
    lv_obj_set_size(s_splash, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_color(s_splash, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(s_splash, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(s_splash, 0, 0);
    lv_obj_set_style_radius(s_splash, 0, 0);
    lv_obj_set_style_pad_all(s_splash, 0, 0);
    lv_obj_clear_flag(s_splash, LV_OBJ_FLAG_SCROLLABLE);

    /*
     * 图是 320x240 的裸像素数组（tools/gen_splash_logo.py 从 assets/logo2.jpg
     * 生成），直接挂上去，不做任何解码——启动页的意义就是盖住启动那段时间，
     * 在这儿解一次 JPEG 等于把空白又还回去了。
     *
     * 尺寸正好等于屏幕，所以居中即铺满；外层 s_splash 的黑色底只是保险，
     * 万一图比屏幕小也不会露出花屏。
     */
    lv_obj_t *logo = lv_img_create(s_splash);
    lv_img_set_src(logo, &splash_logo);
    lv_obj_center(logo);

    s_splash_timer = lv_timer_create(splash_timer_cb, SPLASH_HOLD_MS, NULL);
    lv_timer_set_repeat_count(s_splash_timer, 1);   /* 一次性 */
}

/*---------------------------------------------------------------------------
 * 页面切换
 *-------------------------------------------------------------------------*/
static void switch_page_locked(ui_page_id_t page)
{
    if (page >= UI_PAGE_COUNT || s_pages[page].create == NULL) {
        return;
    }
    if (page == s_current_id && s_current != NULL) {
        return;
    }

    /* 万一有别的路径抢在启动页结束前切页，先把启动页收掉 */
    splash_dismiss_locked();

    /* 先让页面自己清理（停预览、注销回调），再销毁控件 */
    if (s_current_destroy != NULL) {
        s_current_destroy();
        s_current_destroy = NULL;
    }
    if (s_current != NULL) {
        lv_obj_del(s_current);
        s_current = NULL;
    }

    s_current = s_pages[page].create(lv_scr_act());
    s_current_destroy = s_pages[page].destroy;
    s_current_id = page;
}

void ui_goto(ui_page_id_t page)
{
    if (!lvgl_port_lock(0)) {
        return;
    }
    switch_page_locked(page);
    lvgl_port_unlock();
}

/*
 * 待切的目标页。用静态变量而不是每次 malloc 一个结构体：原来那条路径在
 * malloc 失败时是**静默 return**，界面上就是"按了没反应"，很难查。
 * 切页本身幂等（切到当前页会被 switch_page_locked 挡掉），所以只保留
 * 最后一个请求就够了，不需要队列。
 */
static ui_page_id_t s_pending_page;
static bool         s_pending_valid = false;

static void async_goto_cb(void *arg)
{
    (void)arg;
    if (!s_pending_valid) {
        return;
    }
    s_pending_valid = false;
    switch_page_locked(s_pending_page);
}

void ui_goto_async(ui_page_id_t page)
{
    s_pending_page = page;
    s_pending_valid = true;
    /* lv_async_call 的回调由 LVGL 任务在自己的锁内执行 */
    lv_async_call(async_goto_cb, NULL);
}

void ui_go_home(void)
{
    ui_goto(UI_PAGE_HOME);
}

void ui_go_home_async(void)
{
    ui_goto_async(UI_PAGE_HOME);
}

ui_page_id_t ui_current_page(void)
{
    return s_current_id;
}

void ui_open_photos(void)    { ui_goto_async(UI_PAGE_PHOTOS); }
void ui_open_music(void)    { ui_goto_async(UI_PAGE_MUSIC); }
void ui_open_settings(void) { ui_goto_async(UI_PAGE_SETTINGS); }
void ui_open_workmode(void) { ui_goto_async(UI_PAGE_WORKMODE); }
void ui_open_modules(void)  { ui_goto_async(UI_PAGE_MODULES); }
void ui_open_mod_led(void)  { ui_goto_async(UI_PAGE_MOD_LED); }
void ui_open_mod_tank(void) { ui_goto_async(UI_PAGE_MOD_TANK); }
void ui_open_pcmon(void)    { ui_goto_async(UI_PAGE_PCMON); }

/*---------------------------------------------------------------------------
 * 初始化
 *-------------------------------------------------------------------------*/
esp_err_t ui_init(void)
{
    esp_err_t err = bsp_lvgl_start();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "LVGL start failed: %s", esp_err_to_name(err));
        return err;
    }

    if (!lvgl_port_lock(0)) {
        return ESP_FAIL;
    }
    ui_theme_apply_screen(lv_scr_act());
    /* 先出启动页，SPLASH_HOLD_MS 之后由定时器切主菜单。这段时间正好用来
     * 挂 TF 卡（见 app_main 的顺序），主菜单出现时状态栏就是准的。 */
    splash_show_locked();
    /* 自动翻转的轮询：**放在这里而不是某个页面里**——它管的是整幅画面，
     * 而页面会被销毁重建（主菜单、相册、音乐…每个都在的时候都得有效）。
     * 200ms 一拍：方向本来变得就慢，bsp_lvgl_set_flip 又是幂等的 */
    s_rotate_timer = lv_timer_create(rotate_timer_cb, 200, NULL);
    lvgl_port_unlock();

    ESP_LOGI(TAG, "UI ready (splash)");
    return ESP_OK;
}
