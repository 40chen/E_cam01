/*
 * ui_scan.h — WiFi 扫描选网层
 *
 * 盖在设置页上的全屏列表（lv_layer_top()，不是页面），打开即自动扫一次，
 * 扫完按信号强弱列出附近网络。选中一个后回调调用者，由调用者决定接着
 * 弹密码输入还是直接连。
 *
 * 为什么不做成 ui_page_id_t 里的一个页面：返回键要回设置页而不是主菜单，
 * 而 ui_page_shell_create() 的返回键是写死回主菜单的；另外选完网要接着
 * 弹 ui_input_open()，两层都在 lv_layer_top() 上叠着最省事。
 *
 * 代价是它不在 ui.c 的页面生命周期里：设置页销毁时必须自己调
 * ui_scan_close()，否则退出设置页会留下一层盖着主菜单的扫描列表。
 */
#pragma once

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * 选中网络的回调。**在 LVGL 上下文里执行**（已 lv_async_call 回来），
 * 可以安全地开 ui_input_open() 或操作控件。
 * ssid 是 strdup 出来的，用完要 free。
 */
typedef void (*ui_scan_pick_cb_t)(const char *ssid, bool needs_pass, void *user);

/* 打开并开始扫描。已经开着则忽略。 */
void ui_scan_open(ui_scan_pick_cb_t on_pick, void *user);

/* 关掉（幂等，没开也能调）。扫描还在跑的话，结果会被丢弃。 */
void ui_scan_close(void);

bool ui_scan_is_open(void);

#ifdef __cplusplus
}
#endif
