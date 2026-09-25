/*
 * app_module.c — 模块口的编排
 *
 * 这一层很薄，就一件事：记着"现在选的是哪个模块"，并把启用/停止转交给
 * 对应的驱动。**没有检测**——型号是人在界面上点的（见 app_module.h）。
 *
 * 状态**只存 RAM，不写 NVS**：`Secondary Display` 已经立了同样的先例
 * （看不见效果的模式不能掉电自恢复），而且前提本身就是"人为识别"，
 * 每次上电都要人再点一下才对得上眼前实际插的东西。
 */
#include "app_module.h"

#include "esp_log.h"

#include "app_led2812.h"
#include "app_tank.h"

static const char *TAG = "module";

static app_module_id_t s_selected = APP_MODULE_NONE;

typedef struct {
    const char *name;
    bool        implemented;
    esp_err_t (*enable)(void);
    void      (*disable)(void);
} module_desc_t;

/* 表就是"模块列表"本身，页面上那几行是照着它画的。
 * **加新模块只动这张表**：名字、做没做、起停函数，四处 */
static const module_desc_t s_modules[APP_MODULE_COUNT] = {
    [APP_MODULE_LED2812] = { "WS2812 Matrix",   true,  app_led2812_enable, app_led2812_disable },
    [APP_MODULE_TANK]    = { "Tracked Vehicle", true,  app_tank_enable,    app_tank_disable    },
    [APP_MODULE_PRINTER] = { "Printer",         false, NULL,               NULL                },
};

esp_err_t app_module_init(void)
{
    app_module_port_idle();
    ESP_LOGI(TAG, "模块口空闲（GPIO%d / GPIO%d），型号由界面上点选",
             APP_MODULE_GPIO_A, APP_MODULE_GPIO_B);
    return ESP_OK;
}

void app_module_port_idle(void)
{
    const gpio_config_t io = {
        .pin_bit_mask = (1ULL << APP_MODULE_GPIO_A) | (1ULL << APP_MODULE_GPIO_B),
        .mode         = GPIO_MODE_INPUT,
        .pull_up_en   = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    /* 用 gpio_config 而不是 gpio_set_direction：只有它顺手把上拉也设上。
     * 空闲态是"输入 + 内部上拉"，两根脚都撑在 1 上——插着 WS2812 的空板子
     * 读到高不会点亮它（WS2812 判的是高电平的**时长**，静态高不算一帧） */
    ESP_ERROR_CHECK_WITHOUT_ABORT(gpio_config(&io));
}

esp_err_t app_module_select(app_module_id_t id)
{
    if (id == APP_MODULE_NONE) {
        app_module_deselect();
        return ESP_OK;
    }
    if (id < 0 || id >= APP_MODULE_COUNT) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!s_modules[id].implemented) {
        return ESP_ERR_NOT_SUPPORTED;
    }
    if (s_selected == id) {
        return ESP_OK;              /* 幂等：重复点不重启一次硬件 */
    }

    app_module_deselect();          /* 先停旧的 */

    /*
     * **先写状态再 enable**：驱动的 enable() 开头会拿 app_module_selected()
     * 校验"现在轮到我了吗"，顺序反过来它就永远起不来。
     * 这也是"一次只跑一个模块"真正生效的地方——界面不给同时选两个只是
     * 第一道闸，真规矩在这儿。
     */
    s_selected = id;
    const esp_err_t err = s_modules[id].enable();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "%s 启动失败: %s", s_modules[id].name, esp_err_to_name(err));
        s_selected = APP_MODULE_NONE;
        app_module_port_idle();     /* 失败时别把脚留在半配置状态 */
        return err;
    }

    ESP_LOGI(TAG, "模块已启用: %s", s_modules[id].name);
    return ESP_OK;
}

void app_module_deselect(void)
{
    if (s_selected == APP_MODULE_NONE) {
        return;
    }
    const app_module_id_t was = s_selected;
    s_selected = APP_MODULE_NONE;   /* 先摘，驱动再慢也不会被当成"还在跑" */
    s_modules[was].disable();
    ESP_LOGI(TAG, "模块已停止: %s", s_modules[was].name);
}

app_module_id_t app_module_selected(void)
{
    return s_selected;
}

const char *app_module_name(app_module_id_t id)
{
    if (id < 0 || id >= APP_MODULE_COUNT || s_modules[id].name == NULL) {
        return "None";
    }
    return s_modules[id].name;
}

bool app_module_implemented(app_module_id_t id)
{
    if (id < 0 || id >= APP_MODULE_COUNT) {
        return false;
    }
    return s_modules[id].implemented;
}
