# 已移除的功能

这个目录放**不再使用、但结论值得留住**的代码。里面的东西不参与构建，
`main/CMakeLists.txt` 也不再引用它们。工程虽然在 git 里，但删掉之后"当初为什么
不这么做"的结论就跟着没了，所以放这儿。

**这里只留那些"花过大力气才查清、重来一遍很贵"的东西**，不是所有废弃代码都往这儿放。

> 2026-09-26 清过一次：原先存的两条（歌词显示、快门声）连同文件一起删了。快门声那条
> 本来就已经过时（功能后来恢复了，live 代码在 `main/app/shutter_sound.c`），歌词那条
> 结论虽然有用但用户决定不留。两个都还能从 git 历史里翻出来。

---

## USB 副屏（USB Display，2026-09-26 移除）

把整机变成电脑的一块显示器：电脑把桌面扩展过来，电脑那头的驱动把画面压成 JPEG，
通过**原生 USB 口**的 vendor 批量端点推过来，板子解出来整屏画在 ST7789 上
（320x240）。走 `Work Mode → USB Display` 进，靠**按住板上 BOOT 键**退出。

参考实现：`for_example/usb_extend_screen_s3/`（乐鑫 `usb_extend_screen` 例程的
SparkBot 移植版）。

### 这里有什么

| 文件 | 是什么 |
| --- | --- |
| `main_app/app_usbext.c` / `.h` | 全部实现：USB 描述符、收帧状态机、JPEG 解码、分条绘制、退出善后 |

被移除时同步撤掉的**改动**（都已复原，不要以为还在）：

| 位置 | 加过什么 |
| --- | --- |
| `components/lckfb_bsp/` | `bsp_display_get_panel()` ——面板句柄原本是文件静态变量，外面拿不到 |
| `main/ui/ui.c` / `ui.h` | `ui_suspend()` / `ui_resume()` —— 包 `lvgl_port_stop()` / `lvgl_port_resume()` |
| `main/ui/page_workmode.c` | `USB Display` 那一行（可点击的行 + "hold the BOOT button" 说明） |
| `main/app/app_usbmsc.c` | 和 U 盘模式的互斥检查（两个模式抢同一个 USB 口） |
| `sdkconfig` / `sdkconfig.defaults` | `CONFIG_TINYUSB_VENDOR_COUNT=1` 等四项（已回到 0） |

### 为什么移除

**要装一个 Windows IDD 驱动才能用，而那个驱动在目标机器上根本起不来。**

驱动是 Espressif 的预编译签名版（`for_example/usb_extend_screen_s3/windows_driver/`），
按 `USB\VID_303A&PID_2986&MI_00` 匹配。实测到的现象：

```
该设备无法启动。(代码 10)
```

- **USB 2.0 口和 USB 3.0 口都会报 10**
- 显示器**只被认出来过一次**，之后再没成功
- **重装驱动没用**
- 和固件无关（换固件之前就报）

代码 10 是"驱动启动失败"——设备**能**枚举（VID/PID 对得上，硬件 ID 也匹配），但驱动
初始化没起来，所以一帧都不会推。**产品定位上不能接受"用户还得先装个可能装不上的
驱动"**，所以整个功能撤掉。

**注意区分两个不同的失败点**（排查时混过一次，浪费了时间）：

| | 现象 | 含义 |
| --- | --- | --- |
| **代码 10** | 设备管理器里设备带感叹号 | 驱动压根没启动 |
| **没画面** | 驱动起来了、显示器也认了，但板子收不到帧 | 通信/解码/绘制这一侧的问题 |

### 哪些部分是验证过的

**已经确认没问题的**（复活时不用重新怀疑）：

- **USB 描述符全对**。Windows 能枚举出设备、能认到显示器、硬件 ID 是
  `USB\VID_303A&PID_2986&MI_00`，说明 VID/PID/接口号/接口字符串这一整套是对的。
- **接口字符串的格式**：接口 4 那个串按 `..._R<宽>x<高>_Ejpg<质量>` 写，
  实测 `esp32s3udisp0_R320x240_Ejpg6` 能驱动到"认出显示器"这一步。
- **服务和生命周期**：模式进得去、`tinyusb_driver_install()` 成功、
  退出善后（卸 USB → 释放缓冲 → 恢复 LVGL）跑得通。

**从头到尾没验证过的**：一帧都没收到过，所以**收帧、解码、绘制这三段全部没有在
真机上跑通过**。重新捡起来时，别再假设它们是对的。

### 复活时要先试的（按性价比排）

1. **接口字符串里的分辨率**。驱动是从接口 4 那个串里读 `R<宽>x<高>` 来决定建多大的
   显示器。如果它对分辨率有白名单、320x240 不在里面，**初始化阶段就可能直接失败** →
   代码 10。参考项目用的是 `R240x240`（正方形，典型圆屏），那个肯定在白名单里。
   验证：把串临时改成参考项目一模一样的 `esp32p4udisp0_R240x240_Ejpg6` 再试。
2. **Windows 侧的残留绑定**。"只认出来过一次"很像这个——驱动被绑到过一个已经不存在
   的设备实例上，之后每次都拿旧配置启动，重装驱动清不掉。清法（管理员命令行）：

   ```bat
   pnputil /enum-drivers                 :: 找 oemNN.inf 里那个 usb graphic / idd 驱动
   pnputil /delete-driver oemNN.inf /uninstall /force
   ```

   再配合设备管理器 **查看 → 显示隐藏的设备**，把"显示适配器"和"其他设备"下所有
   相关项**连同驱动一起卸载**（卸载对话框里勾"删除此设备的驱动程序软件"），拔插板子。
3. **换一台机器试**。只在一台 Windows 上试过，"只成功一次"也可能是那台机器的状态问题。
4. Windows 那边还要把显示器设成**扩展**或**复制**——设成"仅在 1 上显示"的话那个
   显示器是关的，驱动不会推帧。

### 技术结论（这些和驱动无关，是查源码查实的，复活时直接可用）

#### 协议

帧头 **16 字节**，packed 小端，**照抄参考项目的 `app_vendor.c`，一个字节都不能动**：

```c
typedef struct {
    uint16_t crc16;      /* 设备端不校验 */
    uint8_t  type;       /* 只认 3 = JPG */
    uint8_t  cmd;        /* 设备端不解释 */
    uint16_t x, y;       /* 恒为 0 */
    uint16_t width, height;          /* 必须等于面板尺寸 */
    uint32_t frame_id:10;
    uint32_t payload_total:22;       /* JPEG 字节数，位域高 22 位 */
} __attribute__((packed)) usbext_frame_header_t;
```

> **参考项目的注释里写着"18 字节"，那是数错了**：2+1+1+2+2+2+2 = 12，加 4 字节的
> 位域字 = **16**。存档的代码里有个 `_Static_assert(sizeof(...) == 16)` 盯着它——
> 这个值错了的表现是"画面永远对不上"，在编译期拦下来比在板子上查便宜一万倍。

**没有握手**：电脑枚举完就开始推流，设备端**唯一的流控是丢帧**（没有空缓冲就整帧丢，
单帧超过 32KB 也整帧丢）。设备端的 IN 端点在描述符里必须有，但从不往里写。

#### 收帧：必须写成显式状态机

全速批量端点是 **64 字节**，帧头 16 字节——所以"一次 read 正好给一个帧头"这种假设
**不成立**：帧头可能被切成两半，上一帧的尾巴也可能和下一帧的头挤在同一个 read 里。
参考实现正是栽在这个假设上（它只看 `read` 的第一段）。

正确的形状是 `HEADER(16B) → PAYLOAD(payload_total) → SKIP` 三态，每次 read 带着
**偏移量**往前吃，"本次 read 只能吃下一部分"是正常路径而不是异常。

参考实现里 `frame_add_data()` 失败时那个**空的 if 体**是个真 bug（帧被静默截断），
不要照抄。

#### USB 栈：不用引第二套

`espressif/esp_tinyusb` **自带 vendor 类**（`CONFIG_TINYUSB_VENDOR_COUNT`），和已有的
U 盘模式**共用一份栈**，各自装自己的描述符就行。不需要参考项目那套裸 TinyUSB。

**两个类同时编译进去是安全的**：TinyUSB 0.21 的类驱动是按**当前生效的配置描述符**
逐接口打开的（`vendor_device.c` 的 `vendord_open`），描述符里没有的类根本不会被碰。

几个易踩的细节：

- **缓冲模式要求 RX 和 TX 都非 0**：TinyUSB 里
  `CFG_TUD_VENDOR_TXRX_BUFFERED = (RX_BUFSIZE > 0 && TX_BUFSIZE > 0)`，
  而 `tud_vendor_n_read()` 只在缓冲模式下存在。我们从不写 IN 端点，但 TX 也必须给个 64。
- **回调签名跟 TinyUSB 版本走**。这一版 `CFG_TUD_API_V0_19_COMPAT` 默认 1，签名是
  `void tud_vendor_rx_cb(uint8_t idx, const uint8_t *buffer, uint16_t bufsize)`
  ——注意是 `uint16_t`。缓冲模式下 `buffer`/`bufsize` 是 NULL/0，数据要用
  `tud_vendor_n_read()` 从 FIFO 里取。

#### 绘制：三个查源码才敢下的结论

1. **整帧不能直接从 PSRAM 递给 `esp_lcd_panel_draw_bitmap()`**。SPI 驱动遇到非 DMA
   可用的 tx 缓冲，会在**每次传输**里 `heap_caps_malloc` 一块同样大的内部内存做
   bounce（IDF 5.1.7 `spi_master.c` 的 `setup_priv_desc`）。一帧 15 万字节那样干必然
   失败或把堆搞碎。**必须自己切成条、memcpy 进内部 DMA 缓冲。**
2. **条高的上限不是"够用就行"，是"板子上验证过能分配出来"**。一开始按 40 行
   （2 × 25,600 字节内部 DMA）要，上板直接 `ESP_ERR_NO_MEM`——**进这个模式时 LVGL
   那两块绘制缓冲还没释放**（`lvgl_port_stop()` 只停刷新，不释放缓冲）。
   最后用 **20 行（12,800 字节）**，因为那是 `lcd_clear_black()` 的 `CHUNK_LINES`
   和 LVGL 的 `BSP_LCD_DRAW_BUFFER_HEIGHT` 都在用的尺寸，且 LVGL 是双缓冲——
   说明它本来就在同时持有 2 × 12,800。
3. **不要注册 `on_color_trans_done`**，也不需要为分条缓冲做同步。`draw_bitmap` 是
   异步的，但那个回调位是 **esp_lvgl_port 占着用来驱动 LVGL flush 的，覆盖掉它
   LVGL 恢复后就永远等不到 flush 完成**。好在不用同步：ST7789 的 `draw_bitmap`
   每次都先用**轮询**方式发 CASET/RASET，而 `panel_io_spi_tx_param` 在发之前会把
   在飞的传输收干净（IDF 5.1.7 `esp_lcd_panel_io_spi.c` 里
   "before issue a polling transaction, need to wait queued transactions finished"），
   所以**下一次 `draw_bitmap` 天然保证上一次已经画完**。2 块缓冲交替就够
   （条数因此必须是**偶数**）。

#### 挂起 LVGL：`lvgl_port_stop()` 的准确语义

- 它做 `lv_timer_enable(false)` + `esp_timer_stop(tick_timer)`，**不停也不删 LVGL
  任务**；任务还在跑，但 `lv_timer_handler()` 直接空转返回，面板不会被碰。
- `lv_tick_inc` 就是那个 esp_timer 的回调，所以 **`lv_tick` 一起冻住**——恢复时
  不会有"一堆定时器同时过期"的雪崩。
- **端口互斥锁不受影响**，别的任务照样能 `lvgl_port_lock()` 进来操作 LVGL 对象。
- **触摸也一起停了**（采样本来就在 LVGL 的定时器里）。所以全屏接管类模式必须留一个
  非触摸的出口。
- 恢复时要**主动 `lv_obj_invalidate(lv_scr_act())`**：LVGL 以为屏幕还是它上次画的
  样子，不 invalidate 就会一直显示着别人画的内容。

#### 全屏接管类模式的通用教训

1. **屏幕"冻在上一页"是预期现象，不是故障**。挂起只停刷新、不清屏，面板会一直显示
   LVGL 最后画的那一帧，直到接管方画出第一帧。所以"停在某页"= "一帧都没画成功"。
2. **唯一的退出手段必须排在所有会失败的步骤之后**。`ui_suspend()` 之前任何一步失败
   都还能正常返回、屏幕上还是好的 + 一句 toast；一旦挂了 LVGL 又没任务在画，用户
   面对的就是一块死屏。
3. **所有阻塞等待都必须有超时**。BOOT 键的轮询挂在渲染任务等帧的超时里，只要有一处
   写成 `portMAX_DELAY`，按键就永远轮不到，设备只能拔电。
4. **善后（卸 USB → 释放缓冲 → 恢复 LVGL）放在任务自己身上**，对外接口只置一个标志。
   这样善后只有一个地方发生，也不用为此多养一个"控制器任务"。
   顺序是硬的：**先卸 TinyUSB 再释放缓冲**，否则 RX 回调可能正在用那块内存。
5. **队列在退出时要删掉、不要留着复用**：退出时 filled 队列里可能还压着没处理的项，
   那些下标永远回不到 empty 队列，每进出一次就少一块可用缓冲——几次之后彻底收不下
   东西，而且**日志里一声不响**。
6. **静默失败最难查，宁可吵**。当时补的三条诊断把"没画面"一刀切成四段，直接可用：
   - 启动时 `USB display running; hold BOOT to exit`
   - 一包都没收到时每 5 秒一条 `no USB data yet (Ns)`（区分"电脑没在推"）
   - 第一包 `first USB data: N bytes, starts xx xx ...`（区分"推了但后半段挂了"）
   - 校验失败时把**实际值**打出来（`frame geometry WxH` —— 这直接告诉我们驱动在推
     什么分辨率，比自己猜快得多）

### 其它零碎

- **和串口的关系比参考项目好**：参考的 SparkBot 只有一个 USB 口，进了模式就得按住
  BOOT 才能再烧录。本板 Type-C 后面是 CH334F HUB，分成 CH340K（→ UART0）和原生 USB
  两条，**副屏模式只占原生 USB，日志照看、`idf.py flash` 照烧**。
- **没做音频（UAC）**：esp_tinyusb 不支持 UAC；而且本板没装麦克风（ES7210 不用），
  UAC 输入无从谈起，UAC 输出又和板上喇叭重复。
- **帧率预期**：ESP32-S3 的原生 USB 是**全速 12 Mbps**（属于 USB 2.0 规范，但没有高速
  PHY），有效吞吐 ~800 KB/s 是上限。320x240 的 JPEG 按质量 6 约 12~20KB 一帧，链路
  本身就把帧率卡在 40~60 帧；再算解码和刷屏，**20~30 FPS 是合理预期**。参考项目宣称的
  60FPS 是 480x480 上说的，那个数字过不了 USB 全速这条线。
- **这台机器的 CH340K 是 `USBSER000`**，COM 号拔插一次就会变（见过 COM15 → COM19）。
  "板子没反应"的第一步是重新查号，不是怀疑板子。查法：
  `reg query HKLM\HARDWARE\DEVICEMAP\SERIALCOMM`。
