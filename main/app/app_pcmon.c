/*
 * app_pcmon.c — 副屏的接收端
 *
 * 上位机每秒写一行 JSON（约 250 字节），格式见 assets/Ecam_display/ 那个
 * Python 脚本里的 _monitor_loop()：
 *
 *   {"type":"status","host":"...","cpu":12.3,"ram":45.6,"gpu":-1,
 *    "net_up":1.2,"net_down":3.4,"battery":-1,
 *    "api":{"base_url":"...","key_set":true,"tokens":123,"cost":1.23,
 *           "balance":45.67},"ts":12345}
 *
 * 解析在接收任务里做，结果整份替换进 s_state（带互斥锁保护）。界面只读快照，
 * 不做任何解析——把 JSON 那套东西留在这一层，界面那边就只是"显示一组数"。
 */
#include "app_pcmon.h"

#include <string.h>

#include "cJSON.h"
#include "driver/uart.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

static const char *TAG = "pcmon";

/* 上位机一帧约 250 字节，768 够宽裕。超过这个长度就整行丢掉：
 * **宁可丢一帧也不要拼半行**，半行解析出来的东西没法信 */
#define PCMON_LINE_MAX  768
#define PCMON_RX_BUF    1024
#define PCMON_BAUD      115200
#define PCMON_UART      UART_NUM_0
#define PCMON_TX_PIN    (43)        /* U0TXD，板子上接 CH340K */
#define PCMON_RX_PIN    (44)        /* U0RXD */
#define PCMON_POLL_MS   200         /* 读不到数据时多久醒一次查停止标志 */

static SemaphoreHandle_t s_lock;
static TaskHandle_t      s_task;
static volatile bool     s_stop;
static pcmon_state_t     s_state;
static int64_t           s_last_rx_us;      /* 0 = 还没收到过 */

/*---------------------------------------------------------------------------
 * 解析
 *-------------------------------------------------------------------------*/
static bool json_num(const cJSON *obj, const char *key, double *out)
{
    const cJSON *it = cJSON_GetObjectItemCaseSensitive(obj, key);
    if (!cJSON_IsNumber(it)) {
        return false;
    }
    *out = it->valuedouble;
    return true;
}

static void handle_line(const char *line)
{
    cJSON *root = cJSON_Parse(line);
    if (root == NULL) {
        /* 不是 JSON 就丢。RX 上正常只有上位机那一种数据，但线太长、接触不良、
         * 上位机正在重启都可能吐出半行——**解析失败是常态，不是故障**，
         * 不要为它报错刷屏（要看的话把日志级别调到 DEBUG） */
        ESP_LOGD(TAG, "drop %u bytes that are not json", (unsigned)strlen(line));
        return;
    }

    /*
     * 整帧重建，**缺的项一律回到默认值，不沿用上一帧**。
     * 沿用的话，上位机哪天少发一个字段，屏幕上会继续显示旧数字，看着像实时
     * 值——那比显示 "--" 危险得多。
     */
    pcmon_state_t st;
    memset(&st, 0, sizeof(st));
    st.gpu = -1;

    double d;
    const cJSON *h = cJSON_GetObjectItemCaseSensitive(root, "host");
    if (cJSON_IsString(h) && h->valuestring != NULL) {
        strlcpy(st.host, h->valuestring, sizeof(st.host));
    }
    if (json_num(root, "cpu", &d))      st.cpu = (float)d;
    if (json_num(root, "ram", &d))      st.ram = (float)d;
    if (json_num(root, "gpu", &d))      st.gpu = (int)d;
    if (json_num(root, "net_up", &d))   st.net_up = (float)d;
    if (json_num(root, "net_down", &d)) st.net_down = (float)d;
    /* battery 不解析，见 app_pcmon.h 的说明 */

    const cJSON *api = cJSON_GetObjectItemCaseSensitive(root, "api");
    if (cJSON_IsObject(api)) {
        /* tokens 上位机是当整数发的（json.dumps 出来没有小数点），
         * 但用 double 接住再转更抗它的格式变化 */
        if (json_num(api, "tokens", &d))  st.tokens = (long long)(d + 0.5);
        if (json_num(api, "cost", &d))    st.cost = (float)d;
        if (json_num(api, "balance", &d)) st.balance = (float)d;
    }

    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_state = st;
    s_last_rx_us = esp_timer_get_time();
    xSemaphoreGive(s_lock);

    cJSON_Delete(root);
}

/*---------------------------------------------------------------------------
 * 接收任务
 *-------------------------------------------------------------------------*/
static void pcmon_task(void *arg)
{
    (void)arg;

    char   line[PCMON_LINE_MAX];
    size_t len = 0;
    bool   over = false;        /* 这一行已经超长，丢到换行为止 */
    uint8_t buf[256];

    while (!s_stop) {
        int n = uart_read_bytes(PCMON_UART, buf, sizeof(buf),
                                pdMS_TO_TICKS(PCMON_POLL_MS));
        for (int i = 0; i < n; i++) {
            const char c = (char)buf[i];
            if (c == '\n') {
                if (!over) {
                    line[len] = '\0';
                    if (len > 0) {
                        handle_line(line);
                    }
                }
                len = 0;
                over = false;
                continue;
            }
            if (c == '\r') {
                continue;               /* 上位机写的是 "\n"，CR 只是以防万一 */
            }
            if (over) {
                continue;
            }
            if (len >= sizeof(line) - 1) {
                over = true;
                ESP_LOGW(TAG, "line too long, dropped");
                continue;
            }
            line[len++] = c;
        }
    }

    /* 只有 s_task 还是自己时才清空：stop 等超时之后可能已经重新 start 过，
     * 那时 s_task 是新任务的句柄，旧任务退出再把它置空会让 stop 以为没人在跑 */
    if (s_task == xTaskGetCurrentTaskHandle()) {
        s_task = NULL;
    }
    vTaskDelete(NULL);
}

/*---------------------------------------------------------------------------
 * 对外接口
 *-------------------------------------------------------------------------*/
esp_err_t app_pcmon_start(void)
{
    if (s_task != NULL) {
        return ESP_OK;
    }
    if (s_lock == NULL) {
        s_lock = xSemaphoreCreateMutex();
        if (s_lock == NULL) {
            return ESP_ERR_NO_MEM;
        }
    }

    /*
     * UART0 同时是控制台，但**收 RX 不影响日志**：日志那条路走的是
     * vfs_uart 的 tx_func（开机时指向 ROM 直写），只有显式调用
     * esp_vfs_dev_uart_use_driver() 才会改成走驱动。我们不去调它，
     * 控制台的写法就一个字节都不动（见 components/vfs/vfs_uart.c）。
     *
     * 反过来说，**千万不能调 esp_vfs_dev_uart_use_driver(UART_NUM_0)**：
     * 那会把控制台的读写都接到这个驱动上，而我们从没打算给它当控制台用。
     */
    esp_err_t err = uart_driver_install(PCMON_UART, PCMON_RX_BUF, 0, 0, NULL, 0);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "uart driver install failed: %s", esp_err_to_name(err));
        return err;
    }

    /* 波特率显式写一遍。开机时是 ROM 按 sdkconfig 配的（115200），这里写出来
     * 是为了"读代码就知道是多少"——它必须和上位机的 115200 对上 */
    uart_config_t cfg = {
        .baud_rate  = PCMON_BAUD,
        .data_bits  = UART_DATA_8_BITS,
        .parity     = UART_PARITY_DISABLE,
        .stop_bits  = UART_STOP_BITS_1,
        .flow_ctrl  = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };
    uart_param_config(PCMON_UART, &cfg);
    uart_set_pin(PCMON_UART, PCMON_TX_PIN, PCMON_RX_PIN,
                 UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);

    /* 清掉上一轮留下的快照：开关刚打开时应该显示"等待上位机"，
     * 而不是上次那组已经过期的数字 */
    xSemaphoreTake(s_lock, portMAX_DELAY);
    memset(&s_state, 0, sizeof(s_state));
    s_state.gpu = -1;
    s_last_rx_us = 0;
    xSemaphoreGive(s_lock);

    s_stop = false;
    if (xTaskCreate(pcmon_task, "pcmon", 4096, NULL, 4, &s_task) != pdPASS) {
        s_task = NULL;
        uart_driver_delete(PCMON_UART);
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG, "listening on UART0 RX (GPIO%d) at %d baud", PCMON_RX_PIN, PCMON_BAUD);
    return ESP_OK;
}

void app_pcmon_stop(void)
{
    if (s_task == NULL) {
        return;
    }
    s_stop = true;

    /* 等任务自己退出再卸驱动：uart_driver_delete 会把驱动内部的缓冲和
     * 信号量都释放掉，而任务可能正卡在 uart_read_bytes 里，那是在释放中的
     * 对象上操作。等不到就**不卸**——留着一个没人读的驱动，远好过踩已经
     * 释放的内存 */
    for (int i = 0; i < 25 && s_task != NULL; i++) {
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    if (s_task != NULL) {
        ESP_LOGE(TAG, "receive task did not exit, uart left installed");
        return;
    }

    uart_driver_delete(PCMON_UART);
    ESP_LOGI(TAG, "stopped");
}

bool app_pcmon_is_running(void)
{
    return s_task != NULL;
}

bool app_pcmon_get(pcmon_state_t *out)
{
    if (out == NULL || s_lock == NULL) {
        return false;
    }

    xSemaphoreTake(s_lock, portMAX_DELAY);
    *out = s_state;
    const int64_t last = s_last_rx_us;
    xSemaphoreGive(s_lock);

    if (last == 0) {
        out->valid = false;
        out->age_ms = 0;
        return false;
    }
    out->valid = true;
    out->age_ms = (uint32_t)((esp_timer_get_time() - last) / 1000);
    return true;
}
