/*
 * app_imu.c — QMI8658：重力方向 → 画面要不要翻 180°
 *
 * 寄存器号来自 QMI8658A 数据手册里那张表（参考项目 LCSZP_handheld 的
 * esp32_s3_szp.h 把它写成了一个从 0 开始的 enum，这里换成显式地址，
 * 免得"数着 enum 猜第几项"）：
 *
 *   WHO_AM_I  0x00  → 0x05 才是这颗芯片
 *   CTRL1     0x02  = 0x40  地址自动增加（12 字节连读要靠它）
 *   CTRL2     0x03  = 0x95  加速度 4g @ 250Hz
 *   CTRL7     0x08  = 0x03  打开加速度和陀螺仪
 *   STATUS0   0x2E  bit0 = 加速度数据就绪
 *   AX_L      0x35  起 12 字节 = ax ay az gx gy gz（小端 int16）
 *
 * **本工程只读加速度**，陀螺仪开着是为了跟参考值保持一致（CTRL7=0x03 是
 * 那块板上验过的值，不改它），读回来的 12 字节里后 6 个直接不用。
 *
 * 判定为什么用"归一化后的比例"而不是直接比 g：加速度量程（±2/4/8g）决定
 * 了 1g 对应多少个 LSB，而 CTRL2 那个 0x95 到底是不是 4g 得靠手册确认。
 * 换成 **axis / |矢量|** 之后就是个纯余弦，和量程无关——量程写错了也只会
 * 让满量程变，方向判定照样对。
 */
#include "app_imu.h"

#include <math.h>
#include <string.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "lckfb_bsp.h"

static const char *TAG = "imu";

#define IMU_ADDR_SA0_LOW    (0x6a)      /* 原理图上的值：SA0 接地 */
#define IMU_ADDR_SA0_HIGH   (0x6b)

#define IMU_REG_WHO_AM_I    (0x00)
#define IMU_REG_CTRL1       (0x02)
#define IMU_REG_CTRL2       (0x03)
#define IMU_REG_CTRL3       (0x04)      /* 陀螺仪的 ODR/量程。**不写它陀螺仪不采样** */
#define IMU_REG_CTRL7       (0x08)
#define IMU_REG_STATUS0     (0x2E)
#define IMU_REG_AX_L        (0x35)
#define IMU_WHO_AM_I_VALUE  (0x05)

/*
 * 判定用的是"重力方向和参考方向的夹角"，**不是某一根轴**。
 *
 * 为什么不用轴：哪根轴在画面正立时朝上，取决于芯片在板上的贴装方向——
 * 那个我在板子外面定不了（第一版就是硬编码了一根轴，上板发现不动）。
 * 改成"和参考方向比"之后，**跟贴装方向彻底无关**：
 *
 *   参考方向 = 用户打开自动翻转那一刻的重力方向（那一刻他正看着屏幕，
 *              姿态就是"正立"的定义），之后会慢慢跟着当前姿态走
 *
 *   夹角 ≤ 60°  → 正立
 *   夹角 ≥ 120° → 倒置（也就是"和参考方向的反方向夹角 ≤ 60°"）
 *   中间那 60°  → **灰区，保持现状**（迟滞带，见下）
 *
 * 副作用是**平放在桌上绕竖直轴转 180° 不会翻屏**：那种转法重力方向根本没变
 * （绕的就是重力轴），本来就测不出来——这不是漏判，是物理。
 */
#define IMU_CONE_DEG        60.0f       /* 接受锥半角；点积门限就是 ±cos60° = ±0.5 */
#define IMU_FLIP_CONFIRM    4           /* 连续 4 拍一致才认（20Hz 下 200ms） */
#define IMU_POLL_MS         50
#define IMU_AXIS_LOG_MS     1000        /* 三根轴多久打一行（调试用） */
#define IMU_AXIS_LOG_WINDOW_MS 15000    /* 开机这 15 秒用 INFO 打，之后降到 DEBUG */
/* 15 秒之后要不要把这一秒的一行提到 INFO：姿态相对**上一次打印**变了这么多
 * 就算数（三根轴位移之和，|Δ| 每根最大 2，0.08 ≈ 5° 出头）。
 * 这样"拿起来转一下"永远有数据，而放着不动就一直安静。 */
#define IMU_AXIS_LOG_MOVE   0.08f

/*
 * 参考方向的自适应系数。**这是这一版最关键的一个数**。
 *
 * 为什么必须自适应：参考方向原来是"开机/打开开关那一刻的姿态"，而上板实测
 * 出来的参考是 (0.08, 0.11, **0.99**)——Z 轴占绝对多数，也就是**采参考时设备
 * 是平放的**（放在桌上）。平放时重力沿着屏幕法线，用户把设备在平面里转
 * 180°，重力方向一点没变，点积永远 +1，判据永远够不到 -0.6 ——
 * 表现就是"自动旋转没用"。
 *
 * 现在参考方向会**慢慢跟着当前姿态走**，所以不管开机时设备摆成什么样，
 * 拿起来正常用一两秒之后参考就对齐到"手持正立"了，之后翻转才判得出来。
 *
 * 0.005 在 20Hz 下时间常数约 10 秒：
 *   - 用户真的翻转一次只要 0.5 秒（10 拍），参考几乎没动，点积照样掉到 -1 ✓
 *   - 而拿起来换个姿势这种几十秒量级的变化，参考跟得上 ✓
 * **判成"倒置"期间冻结自适应**，否则用户一直倒着拿，10 秒后参考也跟着倒过去，
 * 界面就自己翻回来了。
 */
#define IMU_REF_ADAPT_ALPHA 0.005f

/*
 * ---- 为什么把"陀螺仪积分"那条路删掉了（2026-09-25）----
 *
 * 原来这里有一套：某根轴的 |a| > 0.85（重力几乎沿着那根轴）时，认为加速度计
 * 进了盲区，改把那根轴的角速度积分起来，转够 150° 就当翻了一次。
 *
 * **那个说法只对了一半，而那半错把用户真正在做的动作挡在了外面。**
 * 盲区的准确条件是"**旋转轴和重力平行**"，不是"重力沿着某根轴"。可代码只能
 * 看到后者，于是拿它当代理判据——用户一上手就翻车了：
 *
 *   用户报的两个姿态（他要的就是这两个）：
 *     正立 轴 +0.99 +0.01 -0.16   ← 重力沿 +X
 *     倒置 轴 -0.92 +0.10 +0.39   ← 重力沿 -X
 *   他是**绕 Y 轴滚 180°** 在这两个之间翻的，重力方向从 +X 跑到了 -X，
 *   加速度计看得一清二楚。但 |ax| > 0.85 先把这一拍踢给了陀螺仪，
 *   而陀螺仪读的是 `dps[normal_axis]` = **X 轴**角速度——正好是和重力平行
 *   的、这次转动里几乎为零的那根轴。日志里就是"转角累计 +0°"，屏幕不动。
 *
 * 用户要的是"这两个姿态各自 ±60° 以内定死正/反"，那本来就是夹角判据的事，
 * 不需要陀螺仪。删掉之后只剩一条路，也不再有"两条路互相打架"的问题。
 *
 * 代价：设备**平放**在桌上绕竖直轴转 180° 不再翻屏——那种转法重力方向一点
 * 没变，加速度计物理上测不出来（平放时屏幕朝上，也没人在看方向）。
 */

static TaskHandle_t  s_task;
static volatile bool s_stop;
static volatile bool s_ok;
static volatile bool s_enabled = true;
static volatile bool s_upside;
static uint8_t       s_addr = IMU_ADDR_SA0_LOW;

/* 低通后的三根轴（归一化）与它的单位向量 */
static float s_filt[3];
static float s_unit[3];

/* 上一次**打印出来**的姿态（见日志那一段）。和"上一次采样"不是一回事：
 * 这里的基线只在真的打了一行 INFO 时才更新，否则一直静止的设备每拍都在
 * 累积位移、迟早误判成"动过了"。 */
static float s_log_unit[3];

/* 参考方向（"正立"时的重力方向）。**只在任务里读写**，除非 s_ref_req 置上 */
static float         s_ref[3];
static volatile bool s_ref_ready;
static volatile bool s_ref_req;

/*---------------------------------------------------------------------------
 * I2C 小工具（走 BSP 的旧版 driver/i2c.h 那套，和相机 SCCB 共用一条总线）
 *-------------------------------------------------------------------------*/
static esp_err_t imu_read(uint8_t addr, uint8_t reg, uint8_t *buf, size_t len)
{
    return bsp_i2c_read(addr, reg, buf, len);
}

static esp_err_t imu_write(uint8_t addr, uint8_t reg, uint8_t val)
{
    return bsp_i2c_write(addr, reg, val);
}

static esp_err_t imu_probe(uint8_t addr)
{
    uint8_t id = 0;
    const esp_err_t err = imu_read(addr, IMU_REG_WHO_AM_I, &id, 1);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "0x%02x 没有应答: %s", addr, esp_err_to_name(err));
        return err;
    }
    ESP_LOGI(TAG, "0x%02x WHO_AM_I = 0x%02x（期望 0x%02x）", addr, id, IMU_WHO_AM_I_VALUE);
    return (id == IMU_WHO_AM_I_VALUE) ? ESP_OK : ESP_ERR_NOT_FOUND;
}

/*---------------------------------------------------------------------------
 * 自检
 *-------------------------------------------------------------------------*/
static esp_err_t imu_bring_up(void)
{
    /* 先把候选地址都点一遍名。**这一步的输出就是这个功能的诊断报告**：
     * 读不到时能直接看出是地址不对、还是整条总线都没应答 */
    ESP_LOGI(TAG, "开始自检（I2C0，和 PCA9557/触摸/ES8311/SCCB 共用）");

    if (imu_probe(IMU_ADDR_SA0_LOW) == ESP_OK) {
        s_addr = IMU_ADDR_SA0_LOW;
    } else if (imu_probe(IMU_ADDR_SA0_HIGH) == ESP_OK) {
        /* SA0 接高就是 0x6b。原理图上是 0x6a，但板子改版/贴错都会落到这儿，
         * 自动认一下比让人对着"0x6a 没反应"猜省事 */
        s_addr = IMU_ADDR_SA0_HIGH;
        ESP_LOGW(TAG, "用 0x%02x（原理图写的是 0x6a，SA0 可能被拉高了）", s_addr);
    } else {
        ESP_LOGE(TAG, "两个地址都没有 WHO_AM_I = 0x05，翻转功能关闭");
        return ESP_ERR_NOT_FOUND;
    }

    /* 配置。**只写必要的几个**：参考工程还配了一堆 Any/No-Motion 中断
     * （CATL1~4 + CTRL8 + CTRL9），那是给"运动唤醒"用的，翻转用不上 */
    esp_err_t err = imu_write(s_addr, IMU_REG_CTRL1, 0x40);   /* 地址自动增加 */
    if (err != ESP_OK) {
        return err;
    }
    err = imu_write(s_addr, IMU_REG_CTRL7, 0x03);             /* 开加速度+陀螺仪 */
    if (err != ESP_OK) {
        return err;
    }
    err = imu_write(s_addr, IMU_REG_CTRL2, 0x95);             /* 加速度 4g @ 250Hz */
    if (err != ESP_OK) {
        return err;
    }
    /*
     * **CTRL3 是必须写的**：CTRL7 的 gEN 只是打开陀螺仪的使能位，
     * 采样率/量程在 CTRL3 里，不写它 ODR 默认是 0 —— 陀螺仪根本不采样，
     * 读回来永远是 0。
     *
     * 2026-09-25 踩的坑：加"平放时靠陀螺仪判翻转"那条路时，我以为 CTRL7
     * 已经把它开好了（注释里还写着"陀螺仪开着是为了跟参考值保持一致"），
     * 实际上只开了使能、没配采样率，积分一直是 0，所以平放转动屏幕毫无反应。
     * 值是照参考工程 for_example/tank/... 的 CTRL3=0xd5：±512dps @ 250Hz，
     * 对应 IMU_GYRO_LSB_PER_DPS = 32768/512 = 64。
     */
    err = imu_write(s_addr, IMU_REG_CTRL3, 0xd5);
    if (err != ESP_OK) {
        return err;
    }

    ESP_LOGI(TAG, "自检通过：地址 0x%02x", s_addr);
    return ESP_OK;
}

/*---------------------------------------------------------------------------
 * 采样任务
 *-------------------------------------------------------------------------*/
static void imu_task(void *arg)
{
    (void)arg;

    int     confirm = 0;
    int64_t axis_log_us = 0;

    while (!s_stop) {
        uint8_t status = 0;
        if (imu_read(s_addr, IMU_REG_STATUS0, &status, 1) == ESP_OK && (status & 0x01)) {
            uint8_t raw[12];
            if (imu_read(s_addr, IMU_REG_AX_L, raw, sizeof(raw)) == ESP_OK) {
                /*
                 * 小端。12 字节里前 6 个是加速度，后 6 个是陀螺仪。
                 * 陀螺仪那 6 个现在不解析了（见上面"为什么把它删掉"），但仍然
                 * 一次读完 12 字节——这是 CTRL1=0x40 + CTRL7=0x03 下上板验过的
                 * 时序，不为了省 6 个字节去动它。
                 */
                const int16_t ax = (int16_t)((raw[1] << 8) | raw[0]);
                const int16_t ay = (int16_t)((raw[3] << 8) | raw[2]);
                const int16_t az = (int16_t)((raw[5] << 8) | raw[4]);
                const int16_t v[3] = { ax, ay, az };

                const float mag = sqrtf((float)ax * ax + (float)ay * ay + (float)az * az);
                if (mag > 1.0f) {
                    /* 三根轴都低通，各存各的。0.25 的系数在 20Hz 下时间常数约 200ms */
                    for (int i = 0; i < 3; i++) {
                        s_filt[i] += 0.25f * ((float)v[i] / mag - s_filt[i]);
                    }
                    /* 单位向量：判据只用方向，不用大小（模长和量程有关，
                     * CTRL2 那个 0x95 到底是不是 4g 得翻手册，这里绕开它） */
                    const float m2 = sqrtf(s_filt[0] * s_filt[0] + s_filt[1] * s_filt[1] +
                                           s_filt[2] * s_filt[2]);
                    if (m2 > 0.3f) {
                        for (int i = 0; i < 3; i++) {
                            s_unit[i] = s_filt[i] / m2;
                        }

                        /* 用户刚打开开关 → 把当前姿态记成"正立" */
                        if (s_ref_req) {
                            s_ref_req = false;
                            for (int i = 0; i < 3; i++) {
                                s_ref[i] = s_unit[i];
                            }
                            s_ref_ready = true;
                            ESP_LOGI(TAG, "参考方向已记录（%+.2f %+.2f %+.2f）",
                                     s_ref[0], s_ref[1], s_ref[2]);
                        }

                        /*
                         * ---- 判定：当前重力和参考方向的夹角，落在哪个区 ----
                         *
                         *   ≤ 60°   → 正立区
                         *   ≥ 120°  → 倒置区（"和参考的反方向夹角 ≤ 60°"）
                         *   中间    → **灰区：保持现状，什么都不做**
                         *
                         * 60/120 是同一对余弦（±0.5）。中间那 60° 宽的灰区就是
                         * 迟滞带：用户从正立翻到倒置，手必然扫过这一段，在这里
                         * 不改变状态，所以姿态抖动不会让画面来回跳。
                         *
                         * 原来这里还有一条"重力沿某根轴就改走陀螺仪积分"的分支，
                         * 2026-09-25 删掉了——它把用户实际在做的翻转挡在了外面，
                         * 原因见文件中间那段注释。
                         */
                        if (s_ref_ready) {
                            const float dot = s_ref[0] * s_unit[0] + s_ref[1] * s_unit[1] +
                                              s_ref[2] * s_unit[2];
                            /* 先夹到 [-1,1] 再 acos：浮点误差会让 dot 略微出界 */
                            const float ang = acosf(fmaxf(-1.0f, fminf(1.0f, dot))) * 57.2958f;

                            bool want = s_upside;               /* 灰区里保持现状 */
                            if (ang <= IMU_CONE_DEG) {
                                want = false;
                            } else if (ang >= 180.0f - IMU_CONE_DEG) {
                                want = true;
                            }

                            if (want == s_upside) {
                                confirm = 0;
                            } else if (++confirm >= IMU_FLIP_CONFIRM) {
                                confirm = 0;
                                s_upside = want;
                                ESP_LOGI(TAG, "轴 %+.2f %+.2f %+.2f  夹角 %3.0f°  方向翻转 → %s",
                                         s_unit[0], s_unit[1], s_unit[2], ang,
                                         want ? "倒置" : "正立");
                            }

                            /*
                             * 参考方向慢慢跟着当前姿态走（见 IMU_REF_ADAPT_ALPHA）。
                             * **判成倒置期间不跟**，否则用户一直倒着拿，参考会跟着
                             * 倒过去，界面自己翻回来。
                             * 放在判定**之后**：这一拍的决定要用自适应之前的参考。
                             */
                            if (!s_upside) {
                                for (int i = 0; i < 3; i++) {
                                    s_ref[i] += IMU_REF_ADAPT_ALPHA * (s_unit[i] - s_ref[i]);
                                }
                                const float r2 = sqrtf(s_ref[0] * s_ref[0] +
                                                       s_ref[1] * s_ref[1] +
                                                       s_ref[2] * s_ref[2]);
                                if (r2 > 0.1f) {
                                    for (int i = 0; i < 3; i++) {
                                        s_ref[i] /= r2;     /* 保持单位向量 */
                                    }
                                }
                            }
                        }
                    }
                }
            }
        }

        /*
         * 三根轴打一行。**这是排查"翻转不灵"的唯一线索**：
         * 拿起来摆正、再翻 180°，看**夹角**从多少跑到多少——两个门限是
         * 60° 和 120°，只有跨过它们才会翻。
         *
         * INFO 的三种情形：开机 15 秒内（不用改日志级别就能看见启动自检）、
         * 以及**姿态动过了**（拿起来转一下就有一串数字，不用重烧固件去开
         * DEBUG）。放着不动时降到 DEBUG，免得一直刷屏——2026-09-25 用户报
         * "后面再转动屏幕也没有数据了"，就是因为当时只有 15 秒这一个窗口。
         */
        const int64_t now = esp_timer_get_time();
        if (now - axis_log_us >= IMU_AXIS_LOG_MS * 1000) {
            axis_log_us = now;
            float ang = 0.0f;
            if (s_ref_ready) {
                const float dot = s_ref[0] * s_unit[0] + s_ref[1] * s_unit[1] +
                                  s_ref[2] * s_unit[2];
                ang = acosf(fmaxf(-1.0f, fminf(1.0f, dot))) * 57.2958f;
            }
            const float moved = fabsf(s_unit[0] - s_log_unit[0]) +
                                fabsf(s_unit[1] - s_log_unit[1]) +
                                fabsf(s_unit[2] - s_log_unit[2]);
            const bool too_new_to_be_quiet = (now < IMU_AXIS_LOG_WINDOW_MS * 1000);
            if (too_new_to_be_quiet || moved > IMU_AXIS_LOG_MOVE) {
                memcpy(s_log_unit, s_unit, sizeof(s_log_unit));
                /*
                 * 夹角后面直接跟"落在哪个区"，再跟"画面现在是什么状态"。
                 * **灰区那一档必须在日志里看得见**：用户会照着这行问
                 * "我都快翻过来了怎么没反应"，而答案就是夹角还没到 120°。
                 */
                const char *zone = NULL;
                if (!s_ref_ready) {
                    zone = "(还没参考)";
                } else if (ang <= IMU_CONE_DEG) {
                    zone = "正立区";
                } else if (ang >= 180.0f - IMU_CONE_DEG) {
                    zone = "倒置区";
                } else {
                    zone = "灰区";
                }
                if (s_ref_ready) {
                    ESP_LOGI(TAG, "轴 %+.2f %+.2f %+.2f  夹角 %3.0f°  %s  → 画面%s",
                             s_unit[0], s_unit[1], s_unit[2], ang, zone,
                             s_upside ? "倒置" : "正立");
                } else {
                    ESP_LOGI(TAG, "轴 %+.2f %+.2f %+.2f  %s  → 画面%s",
                             s_unit[0], s_unit[1], s_unit[2], zone,
                             s_upside ? "倒置" : "正立");
                }
            } else {
                ESP_LOGD(TAG, "轴 %+.2f %+.2f %+.2f  夹角 %3.0f°  → 画面%s",
                         s_unit[0], s_unit[1], s_unit[2], ang,
                         s_upside ? "倒置" : "正立");
            }
        }

        vTaskDelay(pdMS_TO_TICKS(IMU_POLL_MS));
    }

    s_task = NULL;
    vTaskDelete(NULL);
}

/*---------------------------------------------------------------------------
 * 对外接口
 *-------------------------------------------------------------------------*/
esp_err_t app_imu_init(void)
{
    if (s_task != NULL) {
        return ESP_OK;
    }

    const esp_err_t err = imu_bring_up();
    if (err != ESP_OK) {
        return err;
    }

    memset(s_filt, 0, sizeof(s_filt));
    memset(s_unit, 0, sizeof(s_unit));
    s_upside = false;
    s_stop = false;
    s_ref_ready = false;
    /* 开机就采一次参考方向（此时设备多半是正常摆着的）。用户在设置页
     * 重新打开开关时会再采一次，那一次最准——他正看着屏幕 */
    s_ref_req = true;

    /*
     * 栈 4096（原来是 3072）。**调大的原因**：这个任务里每秒要打一行带 5 个
     * 参数的日志（3 个 %+.2f + 1 个 %+.0f + %s），而 ESP_LOGI 走的是
     * vsnprintf——浮点格式化在 32 位上的栈开销比看上去大得多；再加上后来
     * 加的陀螺仪那条路又多了一层局部变量。栈溢出会 panic，而 panic 的表现
     * 离现场很远（见 2026-09-25 那次排查：崩在 esp_psram_init 这种地方）。
     * 这个任务的栈几乎全是富余的，多给 1KB 不心疼。
     */
    if (xTaskCreatePinnedToCore(imu_task, "imu", 4096, NULL, 3, &s_task, 1) != pdPASS) {
        s_task = NULL;
        return ESP_ERR_NO_MEM;
    }
    s_ok = true;

    ESP_LOGI(TAG, "自动翻转已启动（判定靠与参考方向的夹角，与芯片贴装方向无关）");
    return ESP_OK;
}

bool app_imu_ok(void)
{
    return s_ok && s_task != NULL;
}

bool app_imu_upside_down(void)
{
    /* 开关关掉时恒为 false：调用方拿它去设面板，不该自己再判断一次 */
    return s_enabled && s_upside;
}

void app_imu_set_enabled(bool on)
{
    s_enabled = on;
    if (on) {
        /*
         * **打开的那一刻重新采参考方向**，这是这个功能最要紧的一步：
         * 用户此刻正看着屏幕，他的姿态就是"正立"的定义。
         * 开机时那一采是兜底（那时设备可能是平放在桌上，采到什么算什么）。
         */
        s_upside = false;
        s_ref_ready = false;
        s_ref_req = true;
    } else {
        s_upside = false;
        s_ref_ready = false;
    }
}

bool app_imu_enabled(void)
{
    return s_enabled;
}

const char *app_imu_status_str(void)
{
    if (!s_ok) {
        return "No sensor";
    }
    if (!s_enabled) {
        return "Off";
    }
    return s_upside ? "Upside down" : "On";
}
