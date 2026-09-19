/*
 * lckfb_bsp.h — 立创·实战派 ESP32-S3 开发板（LCKFB-SZPI-ESP32-S3-VA）板级支持
 *
 * 本组件移植自立创官方示例 14-handheld 的 esp32_s3_szp.c/h，硬件初始化参数
 * （LCD SPI 模式与时钟、背光极性、SD 卡引脚、音频 I2S 引脚等）均与该示例一致，
 * 因为这些参数已经在该板子上验证过。
 *
 * 与官方示例的两点差异：
 *  1) 所有初始化函数返回 esp_err_t 而不是在内部用 ESP_ERROR_CHECK 中止。
 *     任何单点硬件失败都只记录日志并返回错误，由 app 层决定如何处理，
 *     避免整机陷入复位循环、屏幕全黑而无法定位问题。
 *  2) 背光只有在 LCD 面板初始化完成、GRAM 刷黑、显示打开之后才点亮，
 *     避免开机瞬间闪一下 GRAM 的上电随机内容（花屏）。但面板初始化失败
 *     时会强制点亮，所以“屏幕亮不亮”依然能区分“背光电路问题”与
 *     “面板初始化问题”。
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "esp_lcd_types.h"
#include "driver/gpio.h"
#include "sdmmc_cmd.h"

#ifdef __cplusplus
extern "C" {
#endif

/*==============================================================================
 * I2C 总线
 * 板上 QMI8658、PCA9557、FT6336 触摸、ES8311 以及摄像头的 SCCB 全部复用这一条
 * 总线。使用 legacy 驱动（driver/i2c.h），因为 esp32-camera 在 IDF < 5.4 上
 * 编译的是 legacy 版 sccb.c，通过 SCCB_Use_Port() 直接复用这里装好的端口。
 *============================================================================*/
#define BSP_I2C_SDA        (GPIO_NUM_1)
#define BSP_I2C_SCL        (GPIO_NUM_2)
#define BSP_I2C_NUM        (0)

/*
 * 400kHz。触摸（FT6336）也挂在这条总线上，而且板子没接触摸中断脚、完全靠
 * 轮询，读一次触摸点的时间直接进入输入延迟。100kHz 下一次要约 3-4ms，
 * 400kHz 降到 1ms 上下，这是纯粹白赚的，不影响任何视觉表现。
 *
 * 总线由 IO1/IO2 上的 1k 上拉（R4/R6）拉高，对 400kHz 绰绰有余，
 * 所以这里放心提速。
 */
#define BSP_I2C_FREQ_HZ    (400000)

esp_err_t bsp_i2c_init(void);
esp_err_t bsp_i2c_write(uint8_t dev_addr, uint8_t reg, uint8_t value);
esp_err_t bsp_i2c_read(uint8_t dev_addr, uint8_t reg, uint8_t *out, size_t len);

/*==============================================================================
 * PCA9557 IO 扩展芯片（@0x19）
 * bit0 -> LCD_CS（低有效，初始化后一直拉低）
 * bit1 -> PA_EN（功放使能，放音时拉高）
 * bit2 -> DVP_PWDN（摄像头掉电，0 = 上电）
 *============================================================================*/
#define BSP_PCA9557_ADDR           (0x19)
#define BSP_PCA9557_REG_INPUT      (0x00)
#define BSP_PCA9557_REG_OUTPUT     (0x01)
#define BSP_PCA9557_REG_POLARITY   (0x02)
#define BSP_PCA9557_REG_CONFIG     (0x03)

#define BSP_PCA9557_LCD_CS         (1u << 0)
#define BSP_PCA9557_PA_EN          (1u << 1)
#define BSP_PCA9557_DVP_PWDN       (1u << 2)

/* 上电默认值：LCD_CS=1（不选中）、PA_EN=0（功放关）、DVP_PWDN=1（摄像头断电）。
 * 摄像头要等 bsp_camera_init() 里显式拉低 bit2 才上电。 */
#define BSP_PCA9557_DEFAULT_OUTPUT (0x05)
/* bit0~bit2 配置为输出，其余保持输入 */
#define BSP_PCA9557_DEFAULT_CONFIG (0xF8)

esp_err_t bsp_pca9557_init(void);
esp_err_t bsp_pca9557_set(uint8_t bit, uint8_t level);
esp_err_t bsp_lcd_cs(uint8_t level);
esp_err_t bsp_pa_en(uint8_t level);
esp_err_t bsp_dvp_pwdn(uint8_t level);

/*==============================================================================
 * LCD（ST7789，320x240，SPI3）
 * 注意：片选由 PCA9557 bit0 控制，不走 ESP32 的 GPIO，因此 esp_lcd 的
 * cs_gpio_num 必须是 GPIO_NUM_NC，由 bsp_display_init() 手动拉低并保持。
 *============================================================================*/
#define BSP_LCD_HOST            (SPI3_HOST)
#define BSP_LCD_H_RES           (320)
#define BSP_LCD_V_RES           (240)
#define BSP_LCD_BITS_PER_PIXEL  (16)

#define BSP_LCD_SPI_MOSI        (GPIO_NUM_40)
#define BSP_LCD_SPI_CLK         (GPIO_NUM_41)
#define BSP_LCD_DC              (GPIO_NUM_39)
#define BSP_LCD_BACKLIGHT       (GPIO_NUM_42)

/* 官方示例用 80MHz + SPI mode 2，这两个参数与常见 ST7789 用法（40MHz + mode 0）
 * 不同，但已在该板实测可用，不要照搬别的例程改写。 */
#define BSP_LCD_PIXEL_CLOCK_HZ  (80 * 1000 * 1000)
#define BSP_LCD_SPI_MODE        (2)

/* LEDC 背光：10bit 分辨率，10bit 满量程 1023 对应引脚输出低电平。
 * 背光驱动是 P 沟道 MOS 高边开关（Q2 SI2301CDS），低电平导通。 */
#define BSP_LCD_LEDC_CH         (LEDC_CHANNEL_0)
#define BSP_LCD_LEDC_TIMER      (LEDC_TIMER_0)
#define BSP_LCD_LEDC_RES        (LEDC_TIMER_10_BIT)
#define BSP_LCD_LEDC_FREQ_HZ    (5000)

/* LVGL 绘制缓冲区高度（320*20*2 = 12.8KB，双缓冲） */
#define BSP_LCD_DRAW_BUF_HEIGHT (20)

/*
 * 只初始化背光 PWM（不碰 I2C、不碰 SPI），**不点亮**。
 * 这是整个启动流程里第一条硬件动作，刻意做成不依赖任何其它外设。
 *
 * 之所以不在这里点亮：面板复位后 GRAM 里是上电随机值，这时亮屏就是花屏。
 * 点亮由 bsp_display_init() 在刷黑 GRAM、打开显示之后完成。面板初始化
 * 失败时 bsp_display_init() 会强制点亮，所以“屏幕亮不亮 = 固件跑没跑起来”
 * 这个判断依据依然成立。
 * 可重复调用。
 */
esp_err_t bsp_backlight_init(void);

/* 初始化 SPI 总线 + 面板（内部会确保背光已初始化）。不涉及 LVGL。 */
esp_err_t bsp_display_init(void);
/* 设置背光亮度，0~100（%） */
esp_err_t bsp_backlight_set(int percent);
/* 初始化 LVGL 端口并挂载 LCD 与触摸。需先调用 bsp_display_init()。 */
esp_err_t bsp_lvgl_start(void);
/* 取 LVGL 显示对象（bsp_lvgl_start 之后有效） */
void *bsp_display_get_lvgl_disp(void);

/*==============================================================================
 * 按键（Boot 键）
 * 原理图 SCH_ESP32-S3-V1_0_1：IO0_BOOT 接按键 SW1，另一端接地，低电平有效，
 * 板上已有上拉。SW2 是 RESET，不走 GPIO。
 * 刻意只提供“读电平”，不注册中断、不起任务：拍一张照的判定需要去抖和
 * “松开才重新武装”的状态机，那属于界面层的交互逻辑，放在 BSP 里反而
 * 不好复用。使用方（page_camera.c）用 lv_timer 轮询即可。
 *============================================================================*/
#define BSP_BOOT_BUTTON_GPIO   (GPIO_NUM_0)

/* 把 Boot 键配成输入（内部上拉再保一层）。可重复调用。 */
esp_err_t bsp_boot_button_init(void);

/* true = 按键正被按住 */
bool bsp_boot_button_pressed(void);

/*==============================================================================
 * TF 卡（SDMMC 1-bit）
 * 原理图接线：IO47 = SD_CLK，IO48 = SD_CMD，IO21 = SD_DAT0。
 * 这三个脚必须显式指定——ESP32-S3 的 SDMMC_SLOT_CONFIG_DEFAULT() 会默认填成
 * GPIO14/GPIO15/GPIO2，而 GPIO2 是整板 I2C 的 SCL，一旦被 SDMMC 接管，
 * I2C 总线与摄像头会同时失效。
 *============================================================================*/
#define BSP_SD_CLK_IO      (GPIO_NUM_47)
#define BSP_SD_CMD_IO      (GPIO_NUM_48)
#define BSP_SD_DAT0_IO     (GPIO_NUM_21)
#define BSP_SD_MOUNT_POINT "/sdcard"

esp_err_t bsp_sdcard_mount(void);
esp_err_t bsp_sdcard_unmount(void);
bool bsp_sdcard_is_mounted(void);

/*
 * 只初始化卡本身（能读写扇区），**不挂 FATFS**。U 盘模式要用裸块设备，
 * 而卸载 FATFS 会把 SDMMC 主机 deinit、卡结构体 free 掉，所以必须重新建一张。
 * 用完调 bsp_sdcard_card_deinit()（或直接 bsp_sdcard_unmount()，它也会收）。
 * 失败立刻返回错误，不会重试——这是从界面点进来的，不能把界面卡住。
 */
esp_err_t bsp_sdcard_card_init(void);
void      bsp_sdcard_card_deinit(void);

/* 取当前这张卡（不管 FATFS 有没有挂着）；没有卡时返回 NULL */
sdmmc_card_t *bsp_sdcard_get_card(void);

/*==============================================================================
 * 摄像头（GC0308，DVP，RGB565 QVGA）
 * 掉电引脚由 PCA9557 bit2 控制，电源打开后需要一点时间才能完成 SCCB 探测，
 * bsp_camera_init() 内部已包含该延时。
 *============================================================================*/
#define BSP_CAM_PIN_XCLK   (GPIO_NUM_5)
#define BSP_CAM_PIN_PCLK   (GPIO_NUM_7)
#define BSP_CAM_PIN_VSYNC  (GPIO_NUM_3)
#define BSP_CAM_PIN_HREF   (GPIO_NUM_46)
#define BSP_CAM_PIN_D0     (GPIO_NUM_16)
#define BSP_CAM_PIN_D1     (GPIO_NUM_18)
#define BSP_CAM_PIN_D2     (GPIO_NUM_8)
#define BSP_CAM_PIN_D3     (GPIO_NUM_17)
#define BSP_CAM_PIN_D4     (GPIO_NUM_15)
#define BSP_CAM_PIN_D5     (GPIO_NUM_6)
#define BSP_CAM_PIN_D6     (GPIO_NUM_4)
#define BSP_CAM_PIN_D7     (GPIO_NUM_9)
#define BSP_CAM_XCLK_FREQ_HZ (24000000)

esp_err_t bsp_camera_init(void);
esp_err_t bsp_camera_deinit(void);

/*==============================================================================
 * 音频（ES8311 DAC + 板上功放）
 * 板上没有把功放使能接到 GPIO，而是 PCA9557 bit1，播放前后用 bsp_pa_en() 控制。
 * ES7210 麦克风本工程不使用，故不初始化。
 *============================================================================*/
#define BSP_AUDIO_I2S_NUM   (I2S_NUM_1)
#define BSP_AUDIO_MCLK      (GPIO_NUM_38)
#define BSP_AUDIO_BCLK      (GPIO_NUM_14)
#define BSP_AUDIO_WS        (GPIO_NUM_13)
#define BSP_AUDIO_DOUT      (GPIO_NUM_45)
#define BSP_AUDIO_DIN       (GPIO_NUM_12)

/*
 * 音量 0~100。esp_codec_dev 的 API 只接受这个范围，超过会直接报参数错。
 *
 * 映射关系（从 es8311.c + esp_codec_dev_vol.c 算出来的，2026-09-20 复核）：
 *   esp_codec_dev_col_calc_hw_gain() = 20·log10(dac/pa) = 20·log10(3.3/5.0) = -3.6 dB
 *   es8311_set_vol() 里是 `db -= hw_gain`，等于把整条曲线整体抬高 3.6 dB
 *   → 寄存器 = (音量曲线 + 3.6 + 95.5) × 2 = 音量 + 98（近似）
 * 所以：
 *   vol 85  → DAC_REG32 = 183 (-4.0 dB)
 *   vol 93  → DAC_REG32 = 191 (**0 dB，满量程**)
 *   vol 100 → DAC_REG32 = 198 (+3.6 dB，**在满量程之上**)
 *
 * **93 以上是给 DAC 加数字增益**，对任何接近满刻度的素材就是硬削顶——
 * 那是白送的失真。所以 bsp_audio_volume_set() 会把它夹在 93。
 * 想让声音更大要靠素材本身的响度，不要再往上推这个寄存器。
 */
#define BSP_AUDIO_DEFAULT_VOLUME (93)
/* 音量上限：再往上就是数字增益（见上面），一律夹住 */
#define BSP_AUDIO_VOLUME_MAX     (93)

/*
 * 开机默认采样率。boot 时先按它把 ES8311 打开，之后播放中允许按素材自己的
 * 采样率切换（bsp_audio_set_fs），所以它不再是"全工程唯一的采样率"。
 *
 * 默认值刻意留在 16000：设置页的 Test Speaker 和那条已经验证过的通路都是按
 * 16kHz 标的定，动它没有好处。
 */
#define BSP_AUDIO_DEFAULT_RATE   (16000)

esp_err_t  bsp_audio_init(void);
/*
 * 重配 I2S 与 ES8311 的采样率/位宽/声道数。rate 为 0 时忽略。
 * 只支持 16bit / 立体声（helix 解码输出恒为 16bit，单声道由播放侧转立体声），
 * 采样率支持 8k~48k 里 coeff_div[] 有对应行的档位，见实现里的 s_supported_rates。
 * 不支持的组合返回 ESP_ERR_NOT_SUPPORTED。
 */
esp_err_t  bsp_audio_set_fs(uint32_t rate, uint32_t bits, int channels);
esp_err_t  bsp_audio_write(void *buf, size_t len, size_t *written, uint32_t timeout_ms);
esp_err_t  bsp_audio_mute(bool enable);
esp_err_t  bsp_audio_volume_set(int volume);
esp_err_t  bsp_audio_volume_get(int *volume);

#ifdef __cplusplus
}
#endif
