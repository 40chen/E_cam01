/*
 * page_workmode.c — 工作模式页
 *
 * 把"会改变整机形态"的开关集中在这儿，而不是散在设置页里。设置页管的是
 * 凭据和参数（填一次就不动），这里的每一项都会让设备换一种工作方式，
 * 用户找它们时想的是"切个模式"，不是"去设置里翻"。
 *
 *   USB Drive          把 TF 卡交给电脑当读卡器（原来挂在设置页 STORAGE 下）
 *   Secondary Display  收上位机推过来的状态，显示在副屏页上
 *
 * **开关状态只存在内存里，掉电回到关**：副屏要上位机一直在那头跑着才有意义，
 * 开机自动把串口收起来、而对面根本没在发，只会让人以为功能坏了。
 * （要持久化的话得给 app_settings 加键，目前还没做。）
 */
#include "page_workmode.h"

#include "esp_log.h"

#include "app_pcmon.h"
#include "ui.h"
#include "ui_theme.h"
#include "ui_usbmsc.h"

static const char *TAG = "page_workmode";

static lv_obj_t *s_sw_pcmon;

/*---------------------------------------------------------------------------
 * 行
 *-------------------------------------------------------------------------*/
static void row_usb_cb(lv_event_t *e)
{
    (void)e;
    /* 进模式 + 弹模式页都在 ui_usbmsc_open() 里；失败（没插卡、建卡失败）
     * 会自己弹提示条。模式页是盖在 lv_layer_top() 上的浮层，关掉就回到本页。
     * 没插卡时也让点——由它弹一句 "No TF card"，比把整行置灰更容易看出原因。 */
    ui_usbmsc_open();
}

static void pcmon_switch_cb(lv_event_t *e)
{
    lv_obj_t *sw = lv_event_get_target(e);
    const bool on = lv_obj_has_state(sw, LV_STATE_CHECKED);

    if (!on) {
        app_pcmon_stop();
        ui_toast_show("Secondary display off", lv_color_white());
        return;
    }

    esp_err_t err = app_pcmon_start();
    if (err != ESP_OK) {
        /* 起不来就把开关拨回去。留一个"看着是开的、其实没在收"的开关，
         * 用户会一直等一个永远不来的画面 */
        lv_obj_clear_state(sw, LV_STATE_CHECKED);
        ui_toast_show(err == ESP_ERR_NO_MEM ? "Out of memory" : esp_err_to_name(err),
                      UI_COLOR_DANGER);
        return;
    }
    ui_toast_show("Listening on the serial port", lv_color_white());
}

static void row_pcstatus_cb(lv_event_t *e)
{
    (void)e;
    if (!app_pcmon_is_running()) {
        ui_toast_show("Turn on Secondary Display first", UI_COLOR_DANGER);
        return;
    }
    ui_open_pcmon();
}

/*---------------------------------------------------------------------------
 * 页面
 *-------------------------------------------------------------------------*/
lv_obj_t *page_workmode_create(lv_obj_t *parent)
{
    /* 必须返回外壳本身，内容区用 ui_page_shell_content 取（见 ui_theme.h） */
    lv_obj_t *shell = ui_page_shell_create(parent, "Work Mode");
    lv_obj_t *content = ui_page_shell_content(shell);

    ui_section_create(content, "MODE");

    ui_list_row_create_clickable(content, "USB Drive", "", true, row_usb_cb, NULL);
    ui_list_note_create(content,
                        "Hand the TF card over to your computer as a card reader.\n"
                        "This device cannot read it until you exit.");

    /* Secondary Display：开关在右，行本身不响应点击——点行和点开关做同一件
     * 事的话，想拖动看列表的人会误触发 */
    lv_obj_t *row = ui_list_row_create(content, "Secondary Display", "", false);
    s_sw_pcmon = lv_switch_create(row);
    lv_obj_set_size(s_sw_pcmon, 44, 22);
    lv_obj_align(s_sw_pcmon, LV_ALIGN_RIGHT_MID, 0, 0);
    lv_obj_add_event_cb(s_sw_pcmon, pcmon_switch_cb, LV_EVENT_VALUE_CHANGED, NULL);
    /* 开关是按 app_pcmon 的实际状态摆的，不是按某个页面变量——离开这一页
     * 之后再回来，它得跟正在跑的东西一致 */
    if (app_pcmon_is_running()) {
        lv_obj_add_state(s_sw_pcmon, LV_STATE_CHECKED);
    }
    ui_list_note_create(content,
                        "Receive this PC's CPU / RAM / network and API usage\n"
                        "over the serial port, shown on the PC Status page.");

    ui_list_row_create_clickable(content, "PC Status", "", true, row_pcstatus_cb, NULL);

    ESP_LOGI(TAG, "work mode page created (pcmon %s)",
             app_pcmon_is_running() ? "on" : "off");
    return shell;
}

void page_workmode_destroy(void)
{
    /* 副屏的接收任务**不在这里停**：它跟的是"模式"，不是"页面"。
     * 进来看一眼开关、切回主菜单，模式应该照旧开着。 */
    s_sw_pcmon = NULL;
}
