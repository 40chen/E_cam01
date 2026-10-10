/*
 * motion.c — 差速电机控制（移植自 for_example/tank/c2_tracked_chassis）
 *
 * 电机驱动板上**没有专用驱动 IC**：每路电机接两个 LEDC 通道（A 正转、B 反转），
 * 靠占空比调速、靠哪一路有输出定方向。所以"负速度"不是负占空比，是换一路。
 *
 * 引脚沿用参考项目那份配置（GPIO4/5/6/7，4kHz / 13bit）。
 * **模块板上的走线要对得上这四个脚**，对不上就改下面四个宏。
 *
 * 和参考实现的两处不同：
 *   1. x / y 和左/右速度**两端都夹**。参考实现只夹了上界，负的没夹，
 *      `-speed * 8192 / 100` 在 speed < -100 时会超 13 位满量程；
 *   2. 去掉了 `m1/m2_coefficient` 那套系数（那是给遥控端做微调用的，
 *      我们两个轮子同型号，用不上）。
 */
#include "motion.h"
#include "esp_check.h"

#include "driver/ledc.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "motion";

#define LEDC_MODE        LEDC_LOW_SPEED_MODE
#define LEDC_TIMER       LEDC_TIMER_0
#define LEDC_DUTY_RES    LEDC_TIMER_13_BIT
#define LEDC_FREQ_HZ     4000
#define DUTY_MAX         8192        /* 13 位满量程 */

/* 左轮 = M1，右轮 = M2。和参考项目 c2_tracked_chassis 一致 */
#define M1_A_IO          (4)
#define M1_B_IO          (5)
#define M2_A_IO          (6)
#define M2_B_IO          (7)

/*
 * **看门狗**：最后一次收到驱动指令起 500ms 没再来，就停车。
 *
 * 参考项目把这段放在 UART 解析里，但那样只有 UART 一个来源。现在有两条
 * 驱动路径（Ecam 的摇杆、手机网页），所以它归**电机**管：任何一条
 * motion_drive() 都会把计时喂上，谁断了都停，谁都别想"忘掉"停车。
 *
 * 这也解释了为什么它必须是"最后一条指令"而不是"有没有人连着"：
 * 线和页面都会断，而车不能等对面先说再见。
 */
#define MOTION_WATCHDOG_MS   500
#define MOTION_WD_TICK_MS    50

static bool   s_ready;
/*
 * 用 32 位毫秒而不是 esp_timer 的 64 位微秒：这是两个任务之间共享的，
 * 32 位在 ESP32 上是单条指令读写（原子），64 位会有读到半个值的窗口——
 * 撕出来的那个数会让看门狗莫名其妙停一次车。毫秒的 32 位约 49 天回绕一次，
 * 而下面对时间差用的是无符号减法，回绕本身也是对的。
 */
static volatile uint32_t s_last_drive_ms;
static volatile bool     s_moving;

/* 定义在下面。motion_init() 要用它建任务，所以先声明一句 */
static void motion_wd_task(void *arg);

static void set_motor(int ch_a, int ch_b, int speed)
{
    /* speed: -100..100。正数走 A，负数走 B，0 两路都关 */
    if (speed >  100) speed =  100;
    if (speed < -100) speed = -100;

    const int mag = (speed < 0) ? -speed : speed;
    const uint32_t duty = (uint32_t)(mag * DUTY_MAX / 100);

    ledc_set_duty(LEDC_MODE, ch_a, (speed > 0) ? duty : 0);
    ledc_update_duty(LEDC_MODE, ch_a);
    ledc_set_duty(LEDC_MODE, ch_b, (speed < 0) ? duty : 0);
    ledc_update_duty(LEDC_MODE, ch_b);
}

esp_err_t motion_init(void)
{
    if (s_ready) {
        return ESP_OK;
    }

    const ledc_timer_config_t timer = {
        .speed_mode      = LEDC_MODE,
        .timer_num       = LEDC_TIMER,
        .duty_resolution = LEDC_DUTY_RES,
        .freq_hz         = LEDC_FREQ_HZ,
        .clk_cfg         = LEDC_AUTO_CLK,
    };
    ESP_RETURN_ON_ERROR(ledc_timer_config(&timer), TAG, "ledc timer");

    const int chans[4] = { LEDC_CHANNEL_0, LEDC_CHANNEL_1, LEDC_CHANNEL_2, LEDC_CHANNEL_3 };
    const int pins[4]  = { M1_A_IO, M1_B_IO, M2_A_IO, M2_B_IO };
    for (int i = 0; i < 4; i++) {
        const ledc_channel_config_t ch = {
            .speed_mode = LEDC_MODE,
            .channel    = chans[i],
            .timer_sel  = LEDC_TIMER,
            .intr_type  = LEDC_INTR_DISABLE,
            .gpio_num   = pins[i],
            .duty       = 0,
            .hpoint     = 0,
        };
        ESP_RETURN_ON_ERROR(ledc_channel_config(&ch), TAG, "ledc ch %d", i);
    }

    /* 上电先确保是停的。LEDC 通道刚建出来 duty 是 0，但显式停一下更保险 */
    motion_stop();
    s_ready = true;

    /* 看门狗：栈给 2560 就够（它就两个全局变量和一次 esp_timer 调用） */
    if (xTaskCreate(motion_wd_task, "motor_wd", 2560, NULL, 4, NULL) != pdPASS) {
        ESP_LOGE(TAG, "看门狗任务建不起来——车会一直跑最后一条指令，危险");
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG, "电机就绪（M1=%d/%d M2=%d/%d，%dHz %dbit，看门狗 %dms）",
             M1_A_IO, M1_B_IO, M2_A_IO, M2_B_IO, LEDC_FREQ_HZ, (int)LEDC_DUTY_RES,
             MOTION_WATCHDOG_MS);
    return ESP_OK;
}

/* 看门狗任务：只在"以为在动"的时候才检查，停着的时候一次 I/O 都不做 */
static void motion_wd_task(void *arg)
{
    (void)arg;
    while (1) {
        const uint32_t now = (uint32_t)(esp_timer_get_time() / 1000);
        if (s_moving && (uint32_t)(now - s_last_drive_ms) > MOTION_WATCHDOG_MS) {
            motion_stop();
            s_moving = false;
            ESP_LOGW(TAG, "超过 %dms 没有新的驱动指令，自动停车", MOTION_WATCHDOG_MS);
        }
        vTaskDelay(pdMS_TO_TICKS(MOTION_WD_TICK_MS));
    }
}

void motion_drive(float x, float y)
{
    if (!s_ready) {
        return;
    }
    /*
     * 时间戳每次都喂，但**只有非零指令才把"在动"标上**。
     *
     * 两件事都必要：
     *  - 时间戳要喂：网页那条路是"手指按住不动就不发新指令"吗？不是，
     *    它 100ms 固定发一条。可万一将来改成"变了才发"，持续喂时间戳能让
     *    "手按着不动"不被误判成失控。
     *  - 零指令不设 s_moving：网页开着但没碰的时候一直在发 0,0，设了的话
     *    看门狗每 500ms 就"停车"一次并打一条警告，日志会刷屏。
     */
    s_last_drive_ms = (uint32_t)(esp_timer_get_time() / 1000);
    if (x != 0.0f || y != 0.0f) {
        s_moving = true;
    }
    /* 两端都夹。-1..1 是协议约定，但线上来的数不一定守规矩 */
    if (x < -1.0f) x = -1.0f;
    if (x >  1.0f) x =  1.0f;
    if (y < -1.0f) y = -1.0f;
    if (y >  1.0f) y =  1.0f;

    const float base = y * 100.0f;
    const float turn = x * 100.0f;

    int left  = (int)(base + turn);
    int right = (int)(base - turn);
    if (left  >  100) left  =  100;
    if (left  < -100) left  = -100;
    if (right >  100) right =  100;
    if (right < -100) right = -100;

    set_motor(LEDC_CHANNEL_0, LEDC_CHANNEL_1, left);
    set_motor(LEDC_CHANNEL_2, LEDC_CHANNEL_3, right);
}

void motion_stop(void)
{
    /* 不看 s_ready：init 里也要用它把两路清零，而此时 s_ready 还没置上。
     * 通道没配过的话 ledc_set_duty 会返回错误，那是无害的（返回值不检查） */
    s_moving = false;
    set_motor(LEDC_CHANNEL_0, LEDC_CHANNEL_1, 0);
    set_motor(LEDC_CHANNEL_2, LEDC_CHANNEL_3, 0);
}
