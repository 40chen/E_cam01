/*
 * page_modules.c — 外接模块页
 *
 * 这张列表就是"模块口上能插什么"的清单，**型号由人来点**：点一行 = 把那两根
 * 脚（GPIO10/11）交给对应的驱动，然后进它的控制页。
 *
 * 没有检测、没有"Detected / Not detected"——固件不读那两根脚来判断插了什么
 * （见 app_module.h）。所以这里的每行状态都只是软件状态：`Running` 就是
 * "现在选的是它"。
 *
 * 代价是插拔模块之前要先把当前模块关掉，页面上那句话就是为这个写的。
 */
#include "page_modules.h"

#include "esp_log.h"

#include "app_module.h"
#include "ui.h"
#include "ui_theme.h"

static const char *TAG = "page_modules";

/* 列表里按顺序显示的型号。**加模块就加一行**，其余全在 app_module.c 的表里 */
static const app_module_id_t s_rows[] = {
    APP_MODULE_LED2812,
    APP_MODULE_TANK,
    APP_MODULE_PRINTER,
};

/*---------------------------------------------------------------------------
 * 行回调
 *-------------------------------------------------------------------------*/
static void module_row_cb(lv_event_t *e)
{
    const app_module_id_t id = (app_module_id_t)(intptr_t)lv_event_get_user_data(e);

    const esp_err_t err = app_module_select(id);
    if (err != ESP_OK) {
        ui_toast_show(err == ESP_ERR_NOT_SUPPORTED ? "Not supported yet"
                                                   : esp_err_to_name(err),
                      UI_COLOR_DANGER);
        return;
    }

    /* 进控制页。切页必须异步——这个回调正在执行，整页（含这个按钮）
     * 马上就要被销毁 */
    if (id == APP_MODULE_LED2812) {
        ui_open_mod_led();
    } else if (id == APP_MODULE_TANK) {
        ui_open_mod_tank();
    }
}

/*---------------------------------------------------------------------------
 * 页面
 *-------------------------------------------------------------------------*/
lv_obj_t *page_modules_create(lv_obj_t *parent)
{
    lv_obj_t *shell = ui_page_shell_create(parent, "Modules");
    lv_obj_t *content = ui_page_shell_content(shell);

    ui_section_create(content, "MODULE PORT");

    for (size_t i = 0; i < sizeof(s_rows) / sizeof(s_rows[0]); i++) {
        const app_module_id_t id = s_rows[i];
        const bool implemented = app_module_implemented(id);
        const char *value = "";

        if (!implemented) {
            value = "Unsupported";
        } else if (app_module_selected() == id) {
            value = "Running";
        }

        if (!implemented) {
            /* 没做的型号只占个位，说明这条路留着但暂时走不通。
             * 用不可点的那版建行，不带箭头 */
            ui_list_row_create(content, app_module_name(id), value, false);
        } else {
            ui_list_row_create_clickable(content, app_module_name(id), value, true,
                                         module_row_cb, (void *)(intptr_t)id);
        }
    }

    ui_list_note_create(content,
                        "Only one module at a time.\n"
                        "Pick a module here before plugging it in, and disable it "
                        "before swapping.");

    ESP_LOGI(TAG, "modules page created (selected=%s)", app_module_name(app_module_selected()));
    return shell;
}

void page_modules_destroy(void)
{
    /* 没什么要收的：这里只是选择入口，模块本身跟的是"模式"不是"页面"
     * （和副屏那条一样），离开这一页不会把灯关掉、也不会断开履带车 */
}
