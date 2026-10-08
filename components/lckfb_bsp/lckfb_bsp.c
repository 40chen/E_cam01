/*
 * lckfb_bsp.c — 立创·实战派 ESP32-S3 板级支持实现
 *
 * 硬件参数移植自立创官方示例 14-handheld/main/esp32_s3_szp.c（已在该板验证）。
 * 错误处理策略改为「返回错误码 + 记日志」，不在此层中止启动。
 */
#include <stdlib.h>
#include <string.h>

#include "lckfb_bsp.h"

#include "esp_check.h"
#include "esp_log.h"
#include "esp_heap_caps.h"

#include "driver/i2c.h"
#include "driver/ledc.h"
#include "driver/spi_master.h"
#include "driver/i2s_std.h"
#include "driver/sdmmc_host.h"

#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_vendor.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_touch_ft5x06.h"
#include "esp_lvgl_port.h"

#include "esp_codec_dev.h"
#include "esp_codec_dev_defaults.h"
#include "audio_codec_ctrl_if.h"
#include "audio_codec_data_if.h"
#include "audio_codec_gpio_if.h"
#include "audio_codec_if.h"
#include "es8311_codec.h"

#include "esp_camera.h"
#include "esp_vfs_fat.h"
#include "sdmmc_cmd.h"

static const char *TAG = "bsp";

/*==============================================================================
 * I2C
 *============================================================================*/
esp_err_t bsp_i2c_init(void)
{
    i2c_config_t conf = {
        .mode = I2C_MODE_MASTER,
        .sda_io_num = BSP_I2C_SDA,
        .sda_pullup_en = GPIO_PULLUP_ENABLE,
        .scl_io_num = BSP_I2C_SCL,
        .scl_pullup_en = GPIO_PULLUP_ENABLE,
        .master.clk_speed = BSP_I2C_FREQ_HZ,
    };
    ESP_RETURN_ON_ERROR(i2c_param_config(BSP_I2C_NUM, &conf), TAG, "i2c param config failed");
    ESP_RETURN_ON_ERROR(i2c_driver_install(BSP_I2C_NUM, conf.mode, 0, 0, 0), TAG, "i2c driver install failed");
    return ESP_OK;
}

esp_err_t bsp_i2c_write(uint8_t dev_addr, uint8_t reg, uint8_t value)
{
    uint8_t buf[2] = {reg, value};
    return i2c_master_write_to_device(BSP_I2C_NUM, dev_addr, buf, sizeof(buf),
                                      pdMS_TO_TICKS(1000));
}

esp_err_t bsp_i2c_read(uint8_t dev_addr, uint8_t reg, uint8_t *out, size_t len)
{
    return i2c_master_write_read_device(BSP_I2C_NUM, dev_addr, &reg, 1, out, len,
                                        pdMS_TO_TICKS(1000));
}

/*==============================================================================
 * PCA9557 IO 扩展芯片
 *
 * 这块芯片控制着 LCD 片选、功放使能和摄像头电源，是 I2C 上最关键的器件：
 * 它一旦不响应，屏幕注定点不亮。所以每次失败都要留下明确日志。
 *============================================================================*/
esp_err_t bsp_pca9557_set(uint8_t bit, uint8_t level)
{
    uint8_t value = 0;
    esp_err_t err = bsp_i2c_read(BSP_PCA9557_ADDR, BSP_PCA9557_REG_OUTPUT, &value, 1);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "PCA9557 read failed: %s", esp_err_to_name(err));
        return err;
    }
    value = level ? (value | bit) : (value & ~bit);
    err = bsp_i2c_write(BSP_PCA9557_ADDR, BSP_PCA9557_REG_OUTPUT, value);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "PCA9557 write failed: %s", esp_err_to_name(err));
    }
    return err;
}

esp_err_t bsp_lcd_cs(uint8_t level)     { return bsp_pca9557_set(BSP_PCA9557_LCD_CS, level); }
esp_err_t bsp_pa_en(uint8_t level)      { return bsp_pca9557_set(BSP_PCA9557_PA_EN, level); }
esp_err_t bsp_dvp_pwdn(uint8_t level)   { return bsp_pca9557_set(BSP_PCA9557_DVP_PWDN, level); }

esp_err_t bsp_pca9557_init(void)
{
    /* 先写输出寄存器，再把 bit0~2 配成输出。引脚在配置为输出的瞬间就会
     * 输出寄存器里的值，所以顺序不能反。 */
    ESP_RETURN_ON_ERROR(bsp_i2c_write(BSP_PCA9557_ADDR, BSP_PCA9557_REG_OUTPUT,
                                      BSP_PCA9557_DEFAULT_OUTPUT),
                        TAG, "PCA9557 output init failed (chip not responding?)");
    ESP_RETURN_ON_ERROR(bsp_i2c_write(BSP_PCA9557_ADDR, BSP_PCA9557_REG_CONFIG,
                                      BSP_PCA9557_DEFAULT_CONFIG),
                        TAG, "PCA9557 config init failed");
    return ESP_OK;
}

/*==============================================================================
 * 背光（LEDC PWM）
 *============================================================================*/
esp_err_t bsp_backlight_set(int percent)
{
    if (percent < 0) {
        percent = 0;
    } else if (percent > 100) {
        percent = 100;
    }
    /* 10bit 满量程 1023。配合 output_invert=true，1023 对应引脚输出低电平，
     * 也就是背光最亮（见头文件里 P 沟道 MOS 的说明）。 */
    uint32_t duty = (1023u * (uint32_t)percent) / 100u;
    ESP_RETURN_ON_ERROR(ledc_set_duty(LEDC_LOW_SPEED_MODE, BSP_LCD_LEDC_CH, duty),
                        TAG, "backlight set duty failed");
    ESP_RETURN_ON_ERROR(ledc_update_duty(LEDC_LOW_SPEED_MODE, BSP_LCD_LEDC_CH),
                        TAG, "backlight update duty failed");
    return ESP_OK;
}

static bool s_backlight_ready = false;

esp_err_t bsp_backlight_init(void)
{
    if (s_backlight_ready) {
        return ESP_OK;
    }
    const ledc_timer_config_t timer = {
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .duty_resolution = BSP_LCD_LEDC_RES,
        .timer_num = BSP_LCD_LEDC_TIMER,
        .freq_hz = BSP_LCD_LEDC_FREQ_HZ,
        .clk_cfg = LEDC_AUTO_CLK,
    };
    const ledc_channel_config_t channel = {
        .gpio_num = BSP_LCD_BACKLIGHT,
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .channel = BSP_LCD_LEDC_CH,
        .intr_type = LEDC_INTR_DISABLE,
        .timer_sel = BSP_LCD_LEDC_TIMER,
        .duty = 0,
        .hpoint = 0,
        .flags.output_invert = true,
    };
    ESP_RETURN_ON_ERROR(ledc_timer_config(&timer), TAG, "backlight timer config failed");
    ESP_RETURN_ON_ERROR(ledc_channel_config(&channel), TAG, "backlight channel config failed");
    s_backlight_ready = true;

    /* 只配置，不点亮。面板复位后 GRAM 里是上电随机值，这时候把灯点起来
     * 看到的就是一片花屏。点亮交给 bsp_display_init()：它刷黑 GRAM、
     * 打开显示之后才点灯。 */
    return bsp_backlight_set(0);
}

/*==============================================================================
 * 按键（Boot 键，GPIO0，低电平有效）
 *============================================================================*/
static bool s_boot_button_ready = false;

esp_err_t bsp_boot_button_init(void)
{
    if (s_boot_button_ready) {
        return ESP_OK;
    }
    const gpio_config_t cfg = {
        .pin_bit_mask = 1ULL << BSP_BOOT_BUTTON_GPIO,
        .mode = GPIO_MODE_INPUT,
        /* 板上已经有上拉（按键另一端接地），内部再开一个只是多一层保险：
         * GPIO0 是 strapping 脚，上电瞬间的采样电平不该被这里的配置影响 */
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,     /* 轮询就够了，不占用中断资源 */
    };
    ESP_RETURN_ON_ERROR(gpio_config(&cfg), TAG, "boot button gpio config failed");
    s_boot_button_ready = true;
    return ESP_OK;
}

bool bsp_boot_button_pressed(void)
{
    if (!s_boot_button_ready) {
        return false;
    }
    return gpio_get_level(BSP_BOOT_BUTTON_GPIO) == 0;
}

/*==============================================================================
 * LCD + LVGL
 *============================================================================*/
static esp_lcd_panel_io_handle_t s_io_handle = NULL;
static esp_lcd_panel_handle_t    s_panel_handle = NULL;
static esp_lcd_touch_handle_t    s_touch_handle = NULL;
static lv_disp_t                *s_lvgl_disp = NULL;
static lv_indev_t               *s_lvgl_indev = NULL;

static esp_err_t lcd_panel_init(void)
{
    const spi_bus_config_t buscfg = {
        .sclk_io_num = BSP_LCD_SPI_CLK,
        .mosi_io_num = BSP_LCD_SPI_MOSI,
        .miso_io_num = GPIO_NUM_NC,
        .quadwp_io_num = GPIO_NUM_NC,
        .quadhd_io_num = GPIO_NUM_NC,
        /* 官方示例取 320*40*2：够 LVGL 一次刷 40 行即可，无需开满整屏，
         * 能省下 DMA 描述符。 */
        .max_transfer_sz = BSP_LCD_H_RES * 40 * sizeof(uint16_t),
    };
    ESP_RETURN_ON_ERROR(spi_bus_initialize(BSP_LCD_HOST, &buscfg, SPI_DMA_CH_AUTO),
                        TAG, "LCD SPI bus init failed");

    const esp_lcd_panel_io_spi_config_t io_config = {
        .dc_gpio_num = BSP_LCD_DC,
        /* 片选在 PCA9557 上，这里必须留空，否则 esp_lcd 会和扩展芯片抢这个脚 */
        .cs_gpio_num = GPIO_NUM_NC,
        .pclk_hz = BSP_LCD_PIXEL_CLOCK_HZ,
        .lcd_cmd_bits = 8,
        .lcd_param_bits = 8,
        .spi_mode = BSP_LCD_SPI_MODE,
        .trans_queue_depth = 10,
    };
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)BSP_LCD_HOST,
                                                 &io_config, &s_io_handle),
                        TAG, "new panel io failed");

    const esp_lcd_panel_dev_config_t panel_config = {
        .reset_gpio_num = GPIO_NUM_NC,          /* 板上没有接 RST，靠软复位 */
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB,
        .bits_per_pixel = BSP_LCD_BITS_PER_PIXEL,
    };
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_st7789(s_io_handle, &panel_config, &s_panel_handle),
                        TAG, "new ST7789 panel failed");

    ESP_RETURN_ON_ERROR(esp_lcd_panel_reset(s_panel_handle), TAG, "panel reset failed");
    /* 复位之后立刻拉低片选并一直保持——面板收不到片选就会忽略所有指令，
     * 这正是“背光亮但屏幕全黑”的一个典型原因。 */
    ESP_RETURN_ON_ERROR(bsp_lcd_cs(0), TAG, "LCD CS assert failed");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_init(s_panel_handle), TAG, "panel init failed");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_invert_color(s_panel_handle, true), TAG, "panel invert failed");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_swap_xy(s_panel_handle, true), TAG, "panel swap_xy failed");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_mirror(s_panel_handle, true, false), TAG, "panel mirror failed");
    return ESP_OK;
}

/*
 * 把整屏刷成纯黑。
 *
 * 面板复位后 GRAM 里是上电随机值，只要面板处于显示状态、背光又亮着，
 * 屏幕上就是一片花屏——它会一直持续到 LVGL 画出第一帧（中间还隔着挂
 * TF 卡、LVGL 初始化，实测约一秒）。这里先刷黑，把这段窗口变成黑屏。
 *
 * 纯黑在 RGB565 里是全 0，无论 LV_COLOR_16_SWAP 怎么换字节序都是黑，
 * 所以可以直接 memset 而不用管字节序。
 */
static esp_err_t lcd_clear_black(void)
{
    enum { CHUNK_LINES = 20 };      /* 320*20*2 = 12.8KB，在 max_transfer_sz 之内 */
    const size_t px = (size_t)BSP_LCD_H_RES * CHUNK_LINES;

    uint16_t *buf = heap_caps_malloc(px * sizeof(uint16_t), MALLOC_CAP_DMA);
    if (buf == NULL) {
        return ESP_ERR_NO_MEM;
    }
    memset(buf, 0, px * sizeof(uint16_t));

    esp_err_t err = ESP_OK;
    for (int y = 0; y < BSP_LCD_V_RES && err == ESP_OK; y += CHUNK_LINES) {
        err = esp_lcd_panel_draw_bitmap(s_panel_handle, 0, y,
                                        BSP_LCD_H_RES, y + CHUNK_LINES, buf);
    }
    free(buf);
    return err;
}

esp_err_t bsp_display_init(void)
{
    /* 幂等：app_main 在启动最开始就已经配置过（那时只配置不点亮）。
     * 这里再调一次只是确认 PWM 通道可用。 */
    esp_err_t berr = bsp_backlight_init();
    if (berr != ESP_OK) {
        ESP_LOGE(TAG, "backlight init failed: %s", esp_err_to_name(berr));
    }

    esp_err_t err = lcd_panel_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "LCD panel init failed: %s — forcing the backlight on so the "
                 "fault stays visible (bright screen = firmware alive, even here)",
                 esp_err_to_name(err));
        bsp_backlight_set(100);
        return err;
    }

    /*
     * 顺序是这里的关键，反过来就会开机闪一下花屏：
     *
     *   1. 先刷黑 GRAM —— 此时显示还没打开，写进去谁也看不见
     *   2. 再 DISPON    —— 打开显示的瞬间整屏已经是黑的
     *   3. 最后点背光   —— 这时候屏幕内容已经是确定的了
     *
     * 原来写成 DISPON → 刷黑，于是从打开显示到刷完那几十毫秒里，
     * 屏幕上是 GRAM 的上电随机值（那时背光早就亮着了，是启动第一条
     * 硬件动作点起来的），就是开机那一闪而过的花屏。
     */
    err = lcd_clear_black();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "clear panel failed: %s (screen may show noise until first draw)",
                 esp_err_to_name(err));
    }

    err = esp_lcd_panel_disp_on_off(s_panel_handle, true);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "panel display on failed: %s", esp_err_to_name(err));
        bsp_backlight_set(100);
        return err;
    }

    /* GRAM 已刷黑、显示已开，现在点灯不会闪 */
    return bsp_backlight_set(100);
}

void *bsp_display_get_lvgl_disp(void)
{
    return s_lvgl_disp;
}

static esp_err_t touch_init(void)
{
    esp_lcd_touch_config_t tp_cfg = {
        /* 触摸面板与 LCD 是旋转 90° 的关系，x/y 上限要互换 */
        .x_max = BSP_LCD_V_RES,
        .y_max = BSP_LCD_H_RES,
        .rst_gpio_num = GPIO_NUM_NC,
        .int_gpio_num = GPIO_NUM_NC,
        .levels = { .reset = 0, .interrupt = 0 },
        .flags = { .swap_xy = 1, .mirror_x = 1, .mirror_y = 0 },
    };
    esp_lcd_panel_io_handle_t tp_io = NULL;
    esp_lcd_panel_io_i2c_config_t tp_io_cfg = ESP_LCD_TOUCH_IO_I2C_FT5x06_CONFIG();
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_io_i2c((esp_lcd_i2c_bus_handle_t)BSP_I2C_NUM,
                                                 &tp_io_cfg, &tp_io),
                        TAG, "touch panel io create failed");
    ESP_RETURN_ON_ERROR(esp_lcd_touch_new_i2c_ft5x06(tp_io, &tp_cfg, &s_touch_handle),
                        TAG, "FT5x06 touch create failed (not detected?)");
    return ESP_OK;
}

esp_err_t bsp_lvgl_start(void)
{
    ESP_RETURN_ON_FALSE(s_panel_handle != NULL, ESP_ERR_INVALID_STATE, TAG,
                        "call bsp_display_init() before bsp_lvgl_start()");

    lvgl_port_cfg_t lvgl_cfg = ESP_LVGL_PORT_INIT_CONFIG();
    /*
     * 往上提栈。默认只有 4096（esp_lvgl_port 给"轻量 UI"的预设），但本工程
     * 在 LVGL 任务里做的事已经远超这个预期：相册页要跑 esp_jpeg 解码、
     * FATFS 的 opendir/readdir、以及删除后整棵缩略图控件树的重建。
     * 这些都是深调用链，栈溢出会直接复位，而且现场看不出是这个原因。
     */
    lvgl_cfg.task_stack = 6144;
    ESP_RETURN_ON_ERROR(lvgl_port_init(&lvgl_cfg), TAG, "lvgl port init failed");

    const lvgl_port_display_cfg_t disp_cfg = {
        .io_handle = s_io_handle,
        .panel_handle = s_panel_handle,
        .buffer_size = BSP_LCD_H_RES * BSP_LCD_DRAW_BUF_HEIGHT,
        .double_buffer = true,
        .hres = BSP_LCD_H_RES,
        .vres = BSP_LCD_V_RES,
        .monochrome = false,
        /* 必须与 lcd_panel_init() 里的 swap_xy / mirror 保持一致，
         * 否则画面方向对了但触摸坐标会错位。 */
        .rotation = {
            .swap_xy = true,
            .mirror_x = true,
            .mirror_y = false,
        },
        .flags = {
            .buff_dma = true,      /* 注意：buff_dma 与 buff_spiram 不能同时为 true */
            .buff_spiram = false,
        },
    };
    s_lvgl_disp = lvgl_port_add_disp(&disp_cfg);
    ESP_RETURN_ON_FALSE(s_lvgl_disp != NULL, ESP_FAIL, TAG, "lvgl add disp failed");

    if (touch_init() == ESP_OK) {
        const lvgl_port_touch_cfg_t touch_cfg = {
            .disp = s_lvgl_disp,
            .handle = s_touch_handle,
        };
        s_lvgl_indev = lvgl_port_add_touch(&touch_cfg);
        if (s_lvgl_indev == NULL) {
            ESP_LOGW(TAG, "lvgl add touch failed; UI will be read-only");
        }
    } else {
        ESP_LOGW(TAG, "touch not available; UI will be read-only");
    }

    return ESP_OK;
}

/* 当前是不是翻着的。面板上电时是 (mirror_x=true, mirror_y=false)，
 * "翻过来"就是把它取反——MX/MY 同时反 = 旋转 180° */
static bool s_flipped = false;

esp_err_t bsp_lvgl_set_flip(bool upside_down)
{
    ESP_RETURN_ON_FALSE(s_panel_handle != NULL, ESP_ERR_INVALID_STATE, TAG,
                        "display not initialized");
    ESP_RETURN_ON_FALSE(s_lvgl_disp != NULL, ESP_ERR_INVALID_STATE, TAG,
                        "call bsp_lvgl_start() first");
    if (upside_down == s_flipped) {
        return ESP_OK;              /* 幂等：每 200ms 轮询一次，别每次都翻 */
    }

    if (!lvgl_port_lock(0)) {
        return ESP_ERR_TIMEOUT;
    }

    /*
     * 面板：走 LVGL 的旋转接口，**不要直接调 esp_lcd_panel_mirror()**。
     *
     * MADCTL 管的是"写进来的像素落到 GRAM 的哪个地址"，不是"屏幕上怎么扫"。
     * 所以只翻 MX/MY 的话，**已经显示着的这一屏一个像素都不会动**——必须让
     * LVGL 把整屏重画一遍，新写进去的像素才会按新的映射落位。只改寄存器不
     * 重画，看上去就是"翻转没生效"。
     *
     * lv_disp_set_rotation() → lv_disp_drv_update() 正好把两件事一起做了，
     * 结尾依次是：整屏失效 + 全树 layout 失效 + drv_update_cb。而
     * esp_lvgl_port 的 drv_update_cb 按 disp_cfg.rotation（= swap_xy/mirror_x
     * /mirror_y = true/true/false，和 lcd_panel_init() 里写死的那组一致）算出
     * ROT_180 的镜像位 = (!true, !false) = (false, true)，正是我们要的 180°。
     *
     * 另外：LVGL 只在 sw_rotate 置位时才做软件旋转，而 esp_lvgl_port 从不设它，
     * 所以这里的 rotated 标志**只**驱动 MADCTL，画布依旧是 320x240，没有额外
     * 的每帧重绘开销。
     */
    lv_disp_set_rotation(s_lvgl_disp, upside_down ? LV_DISP_ROT_180 : LV_DISP_ROT_NONE);

    /*
     * 触摸：FT6336 是**另一颗芯片**，面板翻它不会跟着翻，得单独告诉它，
     * 否则画面对着、手指却按在镜像的位置上。
     * 它的初值和面板一致（swap_xy=1, mirror_x=1, mirror_y=0），所以加同样的
     * 180° 就行——两边永远同步，这正是"改一个必须改另一个"的地方。
     *
     * 这里不能指望 esp_lvgl_port 代劳：它的触摸读回调只看得到
     * esp_lcd_touch_get_coordinates() 的结果，完全不看 driver->rotated
     * （只有 USB HID 鼠标那条路会自己按 rotated 换算）。
     */
    if (s_touch_handle != NULL) {
        esp_lcd_touch_set_mirror_x(s_touch_handle, !upside_down);
        esp_lcd_touch_set_mirror_y(s_touch_handle, upside_down);
    }

    lvgl_port_unlock();

    s_flipped = upside_down;
    ESP_LOGI(TAG, "画面%s", upside_down ? "翻转 180°" : "恢复正立");
    return ESP_OK;
}

/*==============================================================================
 * TF 卡
 *============================================================================*/
static sdmmc_card_t *s_card = NULL;
static bool s_sd_mounted = false;

/*
 * host + slot 的配置**只有这一份**，两条路共用：挂 FATFS（bsp_sdcard_mount）
 * 和只建卡不挂文件系统（bsp_sdcard_card_init，U 盘模式用）。
 * 各写一份迟早会改漏一处，而这里改漏的代价很大——见下面引脚的注释。
 */
static void sdcard_config(sdmmc_host_t *host, sdmmc_slot_config_t *slot)
{
    /*
     * 这两个宏都展开成 `{ ... }` 花括号初始化列表，**只能在声明时用**，
     * 不能写成 `*host = SDMMC_HOST_DEFAULT();`（那不是合法的 C）。
     * 所以先各自声明一个，再拷出去。
     */
    const sdmmc_host_t h = SDMMC_HOST_DEFAULT();       /* slot 1 */
    const sdmmc_slot_config_t s = SDMMC_SLOT_CONFIG_DEFAULT();
    *host = h;
    *slot = s;

    /*
     * 必须显式指定引脚。ESP32-S3 的 SDMMC_SLOT_CONFIG_DEFAULT() 会填入
     * clk=GPIO14、cmd=GPIO15、d0=GPIO2，而本板上：
     *   GPIO2  = 整板 I2C 的 SCL（PCA9557 / QMI8658 / 触摸 / ES8311 / 摄像头 SCCB）
     *   GPIO15 = 摄像头 DVP_D4
     *   GPIO14 = I2S_BCLK
     * 挂载失败后这些引脚会一直停在 SDMMC 功能上不恢复，I2C 与摄像头会双双失效。
     */
    slot->clk = BSP_SD_CLK_IO;
    slot->cmd = BSP_SD_CMD_IO;
    slot->d0 = BSP_SD_DAT0_IO;
    slot->d1 = GPIO_NUM_NC;
    slot->d2 = GPIO_NUM_NC;
    slot->d3 = GPIO_NUM_NC;
    slot->d4 = GPIO_NUM_NC;
    slot->d5 = GPIO_NUM_NC;
    slot->d6 = GPIO_NUM_NC;
    slot->d7 = GPIO_NUM_NC;
    slot->width = 1;                                   /* 1-line SD 模式 */
    slot->flags |= SDMMC_SLOT_FLAG_INTERNAL_PULLUP;
}

/* IDF 内部就是这么收尾的：带参数的新版 deinit 和旧版二选一 */
static void sdcard_host_deinit(const sdmmc_host_t *host)
{
    if (host->flags & SDMMC_HOST_FLAG_DEINIT_ARG) {
        host->deinit_p(host->slot);
    } else {
        (*host->deinit)();
    }
}

esp_err_t bsp_sdcard_mount(void)
{
    if (s_sd_mounted) {
        return ESP_OK;
    }

    const esp_vfs_fat_sdmmc_mount_config_t mount_config = {
        .format_if_mount_failed = false,
        .max_files = 8,
        .allocation_unit_size = 8 * 1024,
    };

    sdmmc_host_t host;
    sdmmc_slot_config_t slot;
    sdcard_config(&host, &slot);

    esp_err_t err = esp_vfs_fat_sdmmc_mount(BSP_SD_MOUNT_POINT, &host, &slot,
                                            &mount_config, &s_card);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "TF card mount failed: %s", esp_err_to_name(err));
        s_card = NULL;
        return err;
    }
    s_sd_mounted = true;
    ESP_LOGI(TAG, "TF card mounted at %s", BSP_SD_MOUNT_POINT);
    return ESP_OK;
}

/*
 * 只把卡初始化到"能读扇区"，**不挂 FATFS**。
 *
 * 为什么需要这条独立的路：U 盘模式要把**裸块设备**交给 TinyUSB，而
 * esp_vfs_fat_sdcard_unmount() 内部会 `call_host_deinit(&card->host); free(card);`
 * ——卸载之后主机被 deinit、卡结构体被 free，根本没有东西可以交给 TinyUSB。
 * 所以进 U 盘模式时的顺序是：卸 FATFS → 用这个函数重新建卡 → 交给 TinyUSB。
 *
 * 注意**不要**像官方例程那样 `while (sdmmc_card_init(...))` 无限重试：
 * 这里是从界面点进来的，没插卡就要立刻报错回去，不能把界面卡住。
 */
esp_err_t bsp_sdcard_card_init(void)
{
    if (s_card != NULL) {
        return ESP_OK;
    }

    sdmmc_host_t host;
    sdmmc_slot_config_t slot;
    sdcard_config(&host, &slot);

    sdmmc_card_t *card = malloc(sizeof(sdmmc_card_t));
    ESP_RETURN_ON_FALSE(card != NULL, ESP_ERR_NO_MEM, TAG, "no mem for sdmmc_card_t");

    bool host_up = false;
    esp_err_t err = (*host.init)();
    if (err == ESP_OK) {
        host_up = true;
        err = sdmmc_host_init_slot(host.slot, &slot);
    }
    if (err == ESP_OK) {
        err = sdmmc_card_init(&host, card);
    }
    if (err != ESP_OK) {
        /* 失败要收干净：主机没 deinit 的话，下次进模式 (*host.init)() 会报
         * "already initialized"，引脚也会一直停在 SDMMC 功能上 */
        if (host_up) {
            sdcard_host_deinit(&host);
        }
        free(card);
        ESP_LOGW(TAG, "TF card init failed: %s", esp_err_to_name(err));
        return err;
    }

    s_card = card;
    ESP_LOGI(TAG, "TF card initialized (raw, no filesystem)");
    return ESP_OK;
}

/* 释放 bsp_sdcard_card_init() 建的卡。**只在没挂 FATFS 时用**——
 * 走着 FATFS 的那条路要调 bsp_sdcard_unmount()，它自己会收尾。 */
void bsp_sdcard_card_deinit(void)
{
    if (s_card == NULL) {
        return;
    }
    sdcard_host_deinit(&s_card->host);
    free(s_card);
    s_card = NULL;
}

esp_err_t bsp_sdcard_unmount(void)
{
    if (!s_sd_mounted) {
        /* 卡是用 bsp_sdcard_card_init() 建的（U 盘模式那条路）——这里也顺手
         * 收掉，免得调用方以为"卸载过了"就把卡泄漏在那儿 */
        bsp_sdcard_card_deinit();
        return ESP_OK;
    }
    esp_err_t err = esp_vfs_fat_sdcard_unmount(BSP_SD_MOUNT_POINT, s_card);
    if (err == ESP_OK) {
        s_sd_mounted = false;
        s_card = NULL;
    }
    return err;
}

bool bsp_sdcard_is_mounted(void)
{
    return s_sd_mounted;
}

sdmmc_card_t *bsp_sdcard_get_card(void)
{
    return s_card;
}

/*==============================================================================
 * 摄像头
 *============================================================================*/
static bool s_camera_ready = false;

esp_err_t bsp_camera_init(void)
{
    if (s_camera_ready) {
        return ESP_OK;
    }

    /* 拉低 DVP_PWDN 给摄像头上电，等电源稳定后再做 SCCB 探测 */
    ESP_RETURN_ON_ERROR(bsp_dvp_pwdn(0), TAG, "camera power on failed");
    vTaskDelay(pdMS_TO_TICKS(100));

    camera_config_t config = {
        .ledc_channel = LEDC_CHANNEL_1,
        .ledc_timer = LEDC_TIMER_1,
        .pin_d0 = BSP_CAM_PIN_D0,
        .pin_d1 = BSP_CAM_PIN_D1,
        .pin_d2 = BSP_CAM_PIN_D2,
        .pin_d3 = BSP_CAM_PIN_D3,
        .pin_d4 = BSP_CAM_PIN_D4,
        .pin_d5 = BSP_CAM_PIN_D5,
        .pin_d6 = BSP_CAM_PIN_D6,
        .pin_d7 = BSP_CAM_PIN_D7,
        .pin_xclk = BSP_CAM_PIN_XCLK,
        .pin_pclk = BSP_CAM_PIN_PCLK,
        .pin_vsync = BSP_CAM_PIN_VSYNC,
        .pin_href = BSP_CAM_PIN_HREF,
        /* -1 表示复用 bsp_i2c_init() 已经装好的 I2C 端口，不再单独建 SCCB 总线 */
        .pin_sccb_sda = -1,
        .pin_sccb_scl = BSP_I2C_SCL,
        .sccb_i2c_port = BSP_I2C_NUM,
        .pin_pwdn = -1,          /* 电源由 PCA9557 控制 */
        .pin_reset = -1,
        .xclk_freq_hz = BSP_CAM_XCLK_FREQ_HZ,
        .pixel_format = PIXFORMAT_RGB565,
        /*
         * 按 VGA 分配帧缓冲。GC0308 是 30 万像素的传感器，640x480 才是它的
         * 原生分辨率，拍照要用这个尺寸。
         *
         * 帧缓冲的大小是在这里定死的（esp_camera_init 按 frame_size 分配），
         * 运行时改不了，所以必须一开始就按最大的来——不能等拍照时才发现缓冲
         * 装不下。代价是 2 个 614KB 的缓冲常驻 PSRAM（原来是 2 x 153KB）。
         *
         * 实际输出尺寸在下面用 set_framesize() 降回 QVGA 给预览用。
         */
        .frame_size = FRAMESIZE_VGA,
        .jpeg_quality = 12,
        .fb_count = 2,
        .fb_location = CAMERA_FB_IN_PSRAM,
        .grab_mode = CAMERA_GRAB_WHEN_EMPTY,
    };

    /*
     * Keep PSRAM DMA disabled: this project's 64-byte cache lines are not
     * compatible with the managed camera driver's PSRAM cache-sync alignment.
     * The 16 KB DMA-buffer limit in sdkconfig.defaults keeps the driver's
     * internal staging buffer to 15 KB for VGA RGB565, below available heap.
     */
    esp_err_t err = esp_camera_init(&config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_camera_init failed: %s", esp_err_to_name(err));
        bsp_dvp_pwdn(1);
        return err;
    }

    sensor_t *s = esp_camera_sensor_get();
    if (s != NULL && s->id.PID == GC0308_PID) {
        /* 关闭水平镜像；预览和拍摄照片都使用传感器输出方向。 */
        s->set_hmirror(s, 0);
    }
    /*
     * 传感器**全程**跑 VGA(640x480)，不再切回 QVGA。
     *
     * 曾经试过"缓冲按 VGA 分配、平时输出 QVGA、拍照时切上去"，不成立：
     * cam_hal 在 ISR 里会校验收到帧的长度和配置的 fb_size 是否相等
     * （driver/cam_hal.c），不相等就不把帧入队，而且那块缓冲还会被永久
     * 标记成占用——几帧之后所有缓冲卡死，取帧一直超时，最后中断看门狗复位。
     *
     * 所以只能全程一种尺寸，预览那一路由 app_camera.c 自己在软件里缩一半
     * （downscale_half），效果和传感器抽样出来的 QVGA 差不多。
     */

    s_camera_ready = true;
    ESP_LOGI(TAG, "camera ready");
    return ESP_OK;
}

esp_err_t bsp_camera_deinit(void)
{
    if (!s_camera_ready) {
        return ESP_OK;
    }
    esp_camera_deinit();
    /* SCCB 复用 I2C 端口 0，由 board 层持有，不会被 camera 关掉 */
    bsp_dvp_pwdn(1);
    s_camera_ready = false;
    return ESP_OK;
}

/*==============================================================================
 * 音频（ES8311）
 *============================================================================*/
static esp_codec_dev_handle_t s_play_dev = NULL;
static i2s_chan_handle_t s_i2s_tx = NULL;
static const audio_codec_data_if_t *s_i2s_data_if = NULL;
static const audio_codec_gpio_if_t *s_audio_gpio_if = NULL;
static const audio_codec_ctrl_if_t *s_audio_ctrl_if = NULL;
static const audio_codec_if_t *s_audio_codec_if = NULL;
static uint32_t s_cur_rate = 0;
static int s_volume = BSP_AUDIO_DEFAULT_VOLUME;
static bool s_i2s_tx_enabled;

esp_err_t bsp_audio_deinit(void)
{
    esp_err_t result = ESP_OK;

    if (s_play_dev != NULL) {
        const int close_err = esp_codec_dev_close(s_play_dev);
        if (close_err != ESP_CODEC_DEV_OK) {
            ESP_LOGE(TAG, "codec close failed: %d", close_err);
            result = ESP_FAIL;
        } else if (s_cur_rate != 0) {
            s_i2s_tx_enabled = false;
        }
        esp_codec_dev_delete(s_play_dev);
        s_play_dev = NULL;
    }

    if (s_i2s_tx != NULL && s_i2s_tx_enabled) {
        const esp_err_t err = i2s_channel_disable(s_i2s_tx);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "i2s tx disable failed: %s", esp_err_to_name(err));
            result = err;
        } else {
            s_i2s_tx_enabled = false;
        }
    }

    if (s_audio_codec_if != NULL) {
        if (audio_codec_delete_codec_if(s_audio_codec_if) != ESP_CODEC_DEV_OK) {
            ESP_LOGE(TAG, "codec interface cleanup failed");
            result = ESP_FAIL;
        }
        s_audio_codec_if = NULL;
    }
    if (s_audio_ctrl_if != NULL) {
        if (audio_codec_delete_ctrl_if(s_audio_ctrl_if) != ESP_CODEC_DEV_OK) {
            ESP_LOGE(TAG, "codec control interface cleanup failed");
            result = ESP_FAIL;
        }
        s_audio_ctrl_if = NULL;
    }
    if (s_i2s_data_if != NULL) {
        if (audio_codec_delete_data_if(s_i2s_data_if) != ESP_CODEC_DEV_OK) {
            ESP_LOGE(TAG, "codec data interface cleanup failed");
            result = ESP_FAIL;
        }
        s_i2s_data_if = NULL;
    }
    if (s_audio_gpio_if != NULL) {
        if (audio_codec_delete_gpio_if(s_audio_gpio_if) != ESP_CODEC_DEV_OK) {
            ESP_LOGE(TAG, "codec GPIO interface cleanup failed");
            result = ESP_FAIL;
        }
        s_audio_gpio_if = NULL;
    }
    if (s_i2s_tx != NULL) {
        const esp_err_t err = i2s_del_channel(s_i2s_tx);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "i2s tx delete failed: %s", esp_err_to_name(err));
            result = err;
        } else {
            s_i2s_tx = NULL;
            s_i2s_tx_enabled = false;
        }
    }

    s_cur_rate = 0;
    bsp_pa_en(0);
    return result;
}

esp_err_t bsp_audio_init(void)
{
    if (s_play_dev != NULL && s_i2s_tx != NULL) {
        return ESP_OK;                     /* 已初始化 */
    }

    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(BSP_AUDIO_I2S_NUM, I2S_ROLE_MASTER);
    chan_cfg.auto_clear = true;
    esp_err_t err = i2s_new_channel(&chan_cfg, &s_i2s_tx, NULL);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "i2s new channel failed: %s", esp_err_to_name(err));
        goto fail;
    }

    i2s_std_config_t std_cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(BSP_AUDIO_DEFAULT_RATE),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(32, I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .mclk = BSP_AUDIO_MCLK,
            .bclk = BSP_AUDIO_BCLK,
            .ws   = BSP_AUDIO_WS,
            .dout = BSP_AUDIO_DOUT,
            .din  = BSP_AUDIO_DIN,
        },
    };
    err = i2s_channel_init_std_mode(s_i2s_tx, &std_cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "i2s init std mode failed: %s", esp_err_to_name(err));
        goto fail;
    }

    /*
     * 立刻启用发送通道——和官方示例（11-mp3_player/esp32_s3_szp.c 的
     * bsp_audio_init）一致。
     *
     * 这里以前**故意不调**，注释里写的理由是"esp_codec_dev_open() 要重配槽位和
     * 时钟，而 i2s_channel_reconfig_std_slot/_clock 要求通道处于停止状态"。
     * 那个理由是错的，而且代价很大：
     *
     *   1) esp_codec_dev 在 set_fmt 里**自己会先 _i2s_drv_enable(false)** 把通道
     *      停掉再重配，所以"必须给它留一个停止状态"这个顾虑根本不存在。
     *   2) 一个**从未 enable 过**的通道直接走 reconfig → enable，I2S 的主时钟
     *      起不来。症状极具迷惑性：ES8311 那边寄存器全对（复位已解除、时钟分频
     *      正确、DAC 使能、音量满）、功放也开着，但 i2s_channel_write() 一个字节
     *      都排不进 DMA（1000ms 超时、n=0）——PCM 压根到不了喇叭，能听到的只有
     *      功放使能（PCA9557 bit1）开关那一下"啪"声。
     *
     * 另外，删掉 enable 之后 IDF 会一直打
     *   E i2s_common: i2s_channel_disable: the channel has not been enabled yet
     * 那行 ERROR 本身就是"跟驱动对着干"的迹象，现在它应该消失了。
     */
    err = i2s_channel_enable(s_i2s_tx);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "i2s tx channel enable failed: %s", esp_err_to_name(err));
        goto fail;
    }
    s_i2s_tx_enabled = true;

    audio_codec_i2s_cfg_t i2s_cfg = {
        .port = BSP_AUDIO_I2S_NUM,
        .rx_handle = NULL,
        .tx_handle = s_i2s_tx,
    };
    s_i2s_data_if = audio_codec_new_i2s_data(&i2s_cfg);
    if (s_i2s_data_if == NULL) {
        err = ESP_FAIL;
        ESP_LOGE(TAG, "codec i2s data if failed");
        goto fail;
    }

    s_audio_gpio_if = audio_codec_new_gpio();
    if (s_audio_gpio_if == NULL) {
        err = ESP_FAIL;
        ESP_LOGE(TAG, "codec GPIO interface creation failed");
        goto fail;
    }
    audio_codec_i2c_cfg_t i2c_cfg = {
        .port = BSP_I2C_NUM,
        .addr = ES8311_CODEC_DEFAULT_ADDR,
    };
    s_audio_ctrl_if = audio_codec_new_i2c_ctrl(&i2c_cfg);
    if (s_audio_ctrl_if == NULL) {
        err = ESP_FAIL;
        ESP_LOGE(TAG, "codec i2c ctrl if failed");
        goto fail;
    }

    esp_codec_dev_hw_gain_t gain = {
        .pa_voltage = 5.0,
        .codec_dac_voltage = 3.3,
    };
    es8311_codec_cfg_t es8311_cfg = {
        .ctrl_if = s_audio_ctrl_if,
        .gpio_if = s_audio_gpio_if,
        .codec_mode = ESP_CODEC_DEV_WORK_MODE_DAC,
        /* 功放使能走 PCA9557 而不是 GPIO，所以这里留空，由播放侧调 bsp_pa_en() */
        .pa_pin = GPIO_NUM_NC,
        .pa_reverted = false,
        .master_mode = false,
        .use_mclk = true,
        .digital_mic = false,
        .invert_mclk = false,
        .invert_sclk = false,
        .hw_gain = gain,
    };
    s_audio_codec_if = es8311_codec_new(&es8311_cfg);
    if (s_audio_codec_if == NULL) {
        err = ESP_FAIL;
        ESP_LOGE(TAG, "es8311 codec new failed");
        goto fail;
    }

    esp_codec_dev_cfg_t dev_cfg = {
        .dev_type = ESP_CODEC_DEV_TYPE_OUT,
        .codec_if = s_audio_codec_if,
        .data_if = s_i2s_data_if,
    };
    s_play_dev = esp_codec_dev_new(&dev_cfg);
    if (s_play_dev == NULL) {
        err = ESP_FAIL;
        ESP_LOGE(TAG, "es8311 codec dev new failed");
        goto fail;
    }

    /* 按唯一的那个采样率打开。之后 bsp_audio_set_fs() 再调同样的参数就是空操作，
     * 不会走 close/open —— 那条路径会把 ES8311 的时钟分频器写坏（见 set_fs） */
    err = bsp_audio_set_fs(BSP_AUDIO_DEFAULT_RATE, 16, 2);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "codec open failed: %s", esp_err_to_name(err));
        goto fail;
    }
    bsp_audio_volume_set(s_volume);

    ESP_LOGI(TAG, "audio ready (ES8311)");
    return ESP_OK;

fail:
    bsp_audio_deinit();
    return err;
}

/*
 * 支持运行期切换的采样率。全部取自 esp_codec_dev/device/es8311/es8311.c 的
 * coeff_div[] 里 mclk == rate * 256 的那些行，覆盖 MP3 的全部档位
 * （MPEG1: 32/44.1/48k，MPEG2: 16/22.05/24k，MPEG2.5: 8/11.025/12k）。
 * 表里没有的采样率会在 es8311_config_sample() 里返回 NOT_SUPPORTED，
 * 那会让 esp_codec_dev_open() 半途失败，所以在这里先拦掉。
 */
static const uint32_t s_supported_rates[] = {
    8000, 11025, 12000, 16000, 22050, 24000, 32000, 44100, 48000,
};

/* ES8311 的 7 位 I2C 地址。esp_codec_dev 的 ES8311_CODEC_DEFAULT_ADDR 是 0x30
 * （8 位写法），它内部会 >>1 再交给驱动；而 bsp_i2c_read/write 收的就是 7 位，
 * 所以这里必须移位，不能直接把那个宏传进来。 */
#define ES8311_ADDR_7BIT  (ES8311_CODEC_DEFAULT_ADDR >> 1)

/* 寄存器号抄自 esp_codec_dev/device/es8311/es8311_reg.h —— 那个头文件不在该组件
 * 对外暴露的 include 路径里（只有 include / interface / device/include），
 * 所以在这里写死，改动时对照它。 */
#define ES8311_REG_CLK_MGR02  (0x02)
#define ES8311_REG_CLK_MGR03  (0x03)
#define ES8311_REG_CLK_MGR04  (0x04)
#define ES8311_REG_CLK_MGR05  (0x05)
#define ES8311_REG_CLK_MGR06  (0x06)
#define ES8311_REG_CLK_MGR07  (0x07)
#define ES8311_REG_CLK_MGR08  (0x08)

typedef struct {
    uint8_t reg;
    uint8_t preserve;   /* 写回时要保留的位（对应 es8311_config_sample 里的 &= 掩码） */
    uint8_t expect;     /* 除 preserve 之外那些位的期望值 */
} es8311_clk_reg_t;

static void es8311_expected_clk_regs(uint32_t rate, es8311_clk_reg_t out[7])
{
    /*
     * MCLK = 256 × fs（I2S_STD_CLK_DEFAULT_CONFIG 的 mclk_multiple 默认就是 256），
     * 这个前提下 coeff_div[] 里 8k~48k 各行的 pre_div / pre_multi / adc_div /
     * dac_div / bclk_div / lrck 全都一样，**唯一随采样率变的是 OSR**：
     * 低采样率要用更高的过采样倍数，所以 ≤16kHz 是 0x20，≥22.05kHz 是 0x10。
     * 这也是为什么官方例程（开机 48000）在 44.1k/48k 之间切来切去都没事——
     * 那几档写进去的字节完全相同，它从没跨过"OSR 会变"的这条边界。
     */
    const uint8_t dac_osr = (rate <= 16000) ? 0x20 : 0x10;
    static const es8311_clk_reg_t tpl[7] = {
        { ES8311_REG_CLK_MGR02, 0x07, 0x00 },   /* 预分频 = 1，倍频 = 1 */
        { ES8311_REG_CLK_MGR03, 0x80, 0x10 },   /* 单速模式，ADC OSR = 0x10 */
        { ES8311_REG_CLK_MGR04, 0x80, 0x10 },   /* DAC OSR —— 随采样率变，下面覆盖 */
        { ES8311_REG_CLK_MGR05, 0x00, 0x00 },   /* ADC / DAC 分频 = 1 */
        { ES8311_REG_CLK_MGR06, 0xE0, 0x03 },   /* BCLK 分频 = 4（寄存器里写 4-1） */
        { ES8311_REG_CLK_MGR07, 0xC0, 0x00 },   /* LRCK 分频高字节 */
        { ES8311_REG_CLK_MGR08, 0x00, 0xFF },   /* LRCK 分频低字节，合计 256 */
    };
    memcpy(out, tpl, sizeof(tpl));
    out[2].expect = dac_osr;
}

/*
 * 回读 ES8311 的时钟分频寄存器，和该采样率的期望值比对，不一致就补写。
 *
 * 为什么需要这一步：esp_codec_dev 的 close/open 路径有顺序问题——close 走
 * es8311_suspend()，最后把 RESET_REG00 写成 0x1F（bit7 = CSM 清零，芯片进复位态）；
 * 接着 open 走 codec->set_fs → es8311_config_sample()，上面这 7 个寄存器就是在这时
 * 写的，**此刻芯片还在复位态**；直到 open 末尾的 codec->enable(true) → es8311_start()
 * 才把 CSM 重新拉起来，而 es8311_start() 根本不碰时钟分频器。
 *
 * 所以这几个写到底有没有生效，取决于 ES8311 在 CSM=0 时认不认 I2C 写。光读源码
 * 定不下来（本 BSP 以前那版注释就是凭推理下了"写不进去"的结论，但没有实测过），
 * 而两种答案对应的处理完全相反。回读校验把这个疑问一次问清楚：
 *   - 没有警告日志 → 那几个写本来就是生效的，以前那条结论是错的；
 *   - 有警告日志   → 疑点是真的，这里已经顺手补上，同时日志里留下了证据。
 * 无论哪种，音频都能正常工作；正常情况下这里只是 7 次单字节 I2C 读（约 1ms）。
 */
static void es8311_align_clk_regs(uint32_t rate)
{
    es8311_clk_reg_t want[7];
    es8311_expected_clk_regs(rate, want);

    uint8_t fixed_count = 0;
    for (size_t i = 0; i < sizeof(want) / sizeof(want[0]); i++) {
        uint8_t got = 0;
        if (bsp_i2c_read(ES8311_ADDR_7BIT, want[i].reg, &got, 1) != ESP_OK) {
            ESP_LOGW(TAG, "读 ES8311 REG%02X 失败，跳过本次时钟校验", want[i].reg);
            return;
        }
        if ((uint8_t)(got & ~want[i].preserve) == want[i].expect) {
            continue;
        }
        const uint8_t value = (uint8_t)((got & want[i].preserve) | want[i].expect);
        if (fixed_count == 0) {
            ESP_LOGW(TAG, "%" PRIu32 " Hz：ES8311 时钟寄存器与期望不符，说明 open 里那几个写"
                          "没生效（见 es8311_align_clk_regs 注释），补写：", rate);
        }
        ESP_LOGW(TAG, "  REG%02X: 实际 0x%02X -> 期望 0x%02X", want[i].reg, got, value);
        if (bsp_i2c_write(ES8311_ADDR_7BIT, want[i].reg, value) != ESP_OK) {
            ESP_LOGE(TAG, "  REG%02X 补写失败", want[i].reg);
        }
        fixed_count++;
    }

    if (fixed_count > 0) {
        ESP_LOGW(TAG, "%d 个寄存器已修正。这条日志请保留：它是「复位态写寄存器会丢」"
                      "这个判断的第一个实测证据", fixed_count);
    } else {
        ESP_LOGI(TAG, "%" PRIu32 " Hz：ES8311 时钟寄存器校验通过（7 个全部符合预期）", rate);
    }
}

esp_err_t bsp_audio_set_fs(uint32_t rate, uint32_t bits, int channels)
{
    ESP_RETURN_ON_FALSE(s_play_dev != NULL, ESP_ERR_INVALID_STATE, TAG, "audio not initialized");
    if (rate == 0) {
        return ESP_OK;
    }
    ESP_RETURN_ON_FALSE(bits == 16 && channels == 2, ESP_ERR_NOT_SUPPORTED, TAG,
                        "只支持 16bit/立体声（请求 %" PRIu32 "bit/%dch）", bits, channels);
    if (rate == s_cur_rate) {
        return ESP_OK;          /* 采样率没变——同一档位的歌之间切换走的就是这条路 */
    }
    bool supported = false;
    for (size_t i = 0; i < sizeof(s_supported_rates) / sizeof(s_supported_rates[0]); i++) {
        if (s_supported_rates[i] == rate) {
            supported = true;
            break;
        }
    }
    ESP_RETURN_ON_FALSE(supported, ESP_ERR_NOT_SUPPORTED, TAG,
                        "不支持 %" PRIu32 " Hz（支持档位见 s_supported_rates）", rate);

    /*
     * 换采样率要给 I2S 重配时钟，中间有一小段抖动；不静音的话，DMA 里剩下的
     * 那点残数据会以旧时钟的尾巴放出来，听着是"啪"的一声。
     * 开机第一次打开（s_cur_rate == 0）没有这个问题，也不用多这一次 I2C 写。
     */
    const bool switching = (s_cur_rate != 0);
    if (switching) {
        bsp_audio_mute(true);
    }

    /*
     * 走到这里说明 s_cur_rate == 0，是开机第一次打开，codec 还没 open 过，
     * 这个 close 是空操作（esp_codec_dev_close 见 output_opened 为假会直接返回），
     * 留着只是为了和下面 open 的成对关系清楚。
     *
     * **这段和官方例程 11-mp3_player 的 bsp_codec_set_fs() 是同一套写法**：每个文件
     * 都 close + open，让 I2S 时钟跟着素材自己的采样率走。差别只在于本工程默认是
     * 16000（那条例程默认 48000），第一次切换就会跨过 OSR 那条边界，所以多了一步
     * 回读校验——见 es8311_align_clk_regs 的注释。
     */
    esp_codec_dev_close(s_play_dev);
    if (switching) {
        s_i2s_tx_enabled = false;
    }

    /*
     * I2S 通道的停和启交给 esp_codec_dev 自己管：close() 会走 data_if 的
     * enable(false) 把它停掉，open() 里 set_drv_fs() 重配完槽位和时钟之后再
     * 启起来。**不要自己再调一遍** i2s_channel_disable/enable——重复调只会各
     * 打一条 "has not been enabled yet" 的 ERROR，排查时反而误导。
     *
     * 采样率的时钟也是 open() 按 fs.sample_rate 配的，这里同样不要自己碰
     * i2s_channel_reconfig_std_clock：那个调用要求通道处于停止状态，而此刻
     * 通道已经被 close() 停掉了，自己再配一次既多余又容易撞状态错误。
     */

    esp_codec_dev_sample_info_t fs = {
        .sample_rate = rate,
        .channel = channels,
        .bits_per_sample = bits,
    };
    esp_err_t err = esp_codec_dev_open(s_play_dev, &fs);
    if (err != ESP_OK) {
        /* 失败也要恢复运行，否则一次失败就把音频彻底钉死了 */
        const esp_err_t enable_err = i2s_channel_enable(s_i2s_tx);
        s_i2s_tx_enabled = (enable_err == ESP_OK);
        if (switching) {
            bsp_audio_mute(false);
        }
        /* 标记成"没打开过"：下次调用会走完整的 close/open 重来一遍，
         * 而不是以为 codec 还好好地停在旧采样率上 */
        s_cur_rate = 0;
        ESP_LOGE(TAG, "codec open at %" PRIu32 "Hz/%" PRIu32 "bit/%dch failed: %s",
                 rate, bits, channels, esp_err_to_name(err));
        return err;
    }
    s_i2s_tx_enabled = true;

    /* open 里那几个时钟寄存器写得太早（芯片还在复位态），回读校验一遍。
     * 见 es8311_align_clk_regs 的长注释：正常情况下这里什么都不会做 */
    es8311_align_clk_regs(rate);

    /*
     * 重开之后必须把音量补回去，否则就是全程静音。
     *
     * es8311_close() 内部会走 es8311_suspend()，把 DAC 音量寄存器
     * （ES8311_DAC_REG32）写成 0x00；而 es8311_open() 不会恢复它。
     * 也就是说每次切采样率都会把音量清零，而且**没有任何报错**——
     * PCM 写进去了、功放也开了，就是没声音。
     */
    bsp_audio_volume_set(s_volume);

    if (switching) {
        bsp_audio_mute(false);
    }

    s_cur_rate = rate;
    ESP_LOGI(TAG, "audio fs -> %" PRIu32 " Hz / %" PRIu32 " bit / %d ch", rate, bits, channels);
    return ESP_OK;
}

esp_err_t bsp_audio_write(void *buf, size_t len, size_t *written, uint32_t timeout_ms)
{
    ESP_RETURN_ON_FALSE(s_play_dev != NULL, ESP_ERR_INVALID_STATE, TAG, "audio not initialized");
    ESP_RETURN_ON_FALSE(buf != NULL, ESP_ERR_INVALID_ARG, TAG, "null buf");

    /*
     * 直接走 i2s_channel_write，不用 esp_codec_dev_write()。
     *
     * 原因：esp_codec_dev_write() 返回的是 esp_codec_dev_err_t（0 = OK），**不是
     * 已写字节数**——它内部 _i2s_data_write() 把 i2s_channel_write 的
     * bytes_written 丢掉了，只回 OK/DRV_ERR。也就是说通过它永远问不出"到底写进
     * 去了几个字节"，而这正是排查"没声音"最需要的那个数字。
     *
     * 直接调是**等价**的：_i2s_data_write() 干的就是下面这一句（sw_vol 我们没装，
     * 那个分支不走）。差别只是这里的超时可控，而且能拿到真实字节数。
     */
    if (s_i2s_tx == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    size_t n = 0;
    esp_err_t err = i2s_channel_write(s_i2s_tx, buf, len, &n, timeout_ms);
    if (written) {
        *written = n;
    }
    if (err != ESP_OK) {
        return err;
    }
    /* 短写要当失败报出去，别让调用方以为整段都放完了 */
    return (n == len) ? ESP_OK : ESP_ERR_TIMEOUT;
}

esp_err_t bsp_audio_mute(bool enable)
{
    ESP_RETURN_ON_FALSE(s_play_dev != NULL, ESP_ERR_INVALID_STATE, TAG, "audio not initialized");
    return esp_codec_dev_set_out_mute(s_play_dev, enable);
}

esp_err_t bsp_audio_volume_set(int volume)
{
    ESP_RETURN_ON_FALSE(s_play_dev != NULL, ESP_ERR_INVALID_STATE, TAG, "audio not initialized");
    if (volume < 0) {
        volume = 0;
    } else if (volume > BSP_AUDIO_VOLUME_MAX) {
        /* 93 以上是数字增益（0dBFS 在 93，100 是 +3.6dB）。夹住——
         * 那增益对接近满刻度的素材只会削顶，不是"更响"。见头文件里的推导。 */
        volume = BSP_AUDIO_VOLUME_MAX;
    }
    s_volume = volume;
    return esp_codec_dev_set_out_vol(s_play_dev, volume);
}

esp_err_t bsp_audio_volume_get(int *volume)
{
    ESP_RETURN_ON_FALSE(volume != NULL, ESP_ERR_INVALID_ARG, TAG, "null out");
    *volume = s_volume;
    return ESP_OK;
}
