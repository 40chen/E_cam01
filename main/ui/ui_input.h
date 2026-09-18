/*
 * ui_input.h — 全屏文本输入层（软键盘）
 *
 * 设置页的通用输入界面：标题栏（取消 / 保存 / 密码可见切换）+ 单行输入框
 * + LVGL 软键盘，盖在 lv_layer_top() 上，一次只允许开一个。
 *
 * 回调约定：text == NULL 表示用户取消；否则是 strdup 出来的文本，
 * 用完 free。回调在 LVGL 任务上下文里执行，可以直接操作控件。
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef void (*ui_input_done_cb_t)(const char *text, void *user);

/*
 * 打开输入层。value 为初始文本（可为 NULL），password 走打码模式并在
 * 标题栏显示明文切换，numeric 用纯数字键盘，max_len 限制最大字符数。
 */
void ui_input_open(const char *title, const char *value,
                   bool password, bool numeric, size_t max_len,
                   ui_input_done_cb_t cb, void *user);

#ifdef __cplusplus
}
#endif
