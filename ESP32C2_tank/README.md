# ESP32C2 tank

ESP32-C2 履带车模块的固件。这块板子挂在一辆双电机履带底盘上，**插上两根线只为把
WiFi 凭据交给它**，交完就可以拔掉——之后 Ecam 用 UDP 无线发摇杆数据开车，车自己
起一个网页，手机浏览器打开它的 IP 也能开（两条驱动路径）。

它是 `E_cam/` 工作目录里的四个平行仓库之一（2026-09-23 分的家，各自独立）：

| 同级目录 | 放什么 |
| --- | --- |
| [`ESP32S3_Ecam/`](https://github.com/40chen/ESP32S3_Ecam) | 主机固件（摇杆 + 屏幕）。**本仓库的对端**，协议两边必须一致 |
| `ESP32C2_tank/` | 本仓库：履带车模块固件 |
| `host control/` | 上位机（`Ecam_display/`），不在任何一个仓库里 |
| `for_example/` | 参考项目，只读不改 |

```
Ecam（主机，摇杆 + 屏幕）              C2（本工程，车上）
  ① UART1（只在配网时插着）
     TX ─────────────────────────→ RX    #hi / #wifi 1,2,3（凭据）
     RX ←───────────────────────── TX    #ok / #wifi ok / #ip
  ② UDP 单播 :3333（拔线之后，20Hz）
     ─────────────────────────────→      运动指令 x<f> y<f>
     ←─────────────────────────────      1Hz #ip 心跳
```

**运动指令两条路都收，凭据只认有线那条**（UDP 上收到 `#wifi` 一律回
`#wifi err 4`）——理由见下面"协议"一节。

## 目录

| 文件 | 管什么 |
| --- | --- |
| `src/main.c` | 启动顺序，五步串起来 |
| `src/motion.c` | 差速电机 + 500ms 看门狗 |
| `src/link.c` | 行式解析、base64、凭据事务。**UART 和 UDP 共用这一个解析器** |
| `src/udp.c` | 无线驱动链路：收 20Hz 摇杆包、回 1Hz `#ip` 心跳 |
| `src/wifi_prov.c` | NVS 存凭据、STA 连接、IP 回调 |
| `src/web.c` | HTTP 遥控页面 |
| `boards/` | `esp32-c2-devkitm-1` 的板定义，**项目自带一份**（理由见"构建"） |
| `include/` `lib/` `test/` | PlatformIO 脚手架留下来的空目录，没用到 |

启动顺序是 `motion → link → wifi → udp → web`，**任何一步失败都不中止**：

- 电机先停稳再听指令；
- 链路早早开着（Ecam 一插上就会发 `#hi` 和运动指令）；
- UDP 要在 WiFi 之后，但它建 socket 和收包都不依赖"已经连上"，
  连不上就是收不到东西而已；
- 网页最后起，它要等 IP，而 IP 归 WiFi 管。

车上没有屏幕，"起不来"和"跑起来了但没连上网"必须能从日志里分清，所以这里不用
"一步失败就 return" 那种写法。

## 构建与烧录

PlatformIO + ESP-IDF（`framework = espidf`，不是 Arduino）：

```bash
pio run                  # 构建
pio run -t upload        # 烧录
pio device monitor       # 串口日志，74880
```

`platformio.ini` 里 `upload_speed = 115200`（原先写 460800，掉回默认值更稳），
`monitor_speed = 74880`。ESP32-C2 按参考配置使用 26MHz XTAL 时，ESP-IDF 默认控制台
波特率是 74880；UART1 和 Ecam 通信仍是 115200。

⚠️ **构建目录在 OneDrive 里，会出怪事。** 中断过一次构建之后，下一次大概率报
`FileExistsError [WinError 183]`，栈在 `os.makedirs('.pio/build/<board>')`——
PlatformIO 先 `isdir()` 判断、再 `makedirs()`，而 OneDrive 会在这两步中间**异步**
把目录建出来。删掉 `.pio/build` 重跑即可。

### `src/CMakeLists.txt` 是手写的

PlatformIO 生成的那份只有 `idf_component_register(SRCS ${app_sources})`，
**没有 `REQUIRES`**，于是这个组件只拿得到 IDF 的"公共依赖"（freertos / log / heap…），
而 `nvs_flash` / `esp_wifi` / `esp_netif` / `esp_http_server` 都不在里面——
**编译能过，链接期一片 undefined reference**。这份是手写的，把这几个补上。

`SRCS` 显式列出来而不是 `GLOB_RECURSE`：加文件时多改一行，但不会出现
"新写的 `.c` 没被编进去、却什么错都不报"。

### `boards/` 为什么自带一份

装在这台机器上的几套 `espressif32` 平台（6.9.0 / 6.12.0）里都**没有**
`esp32-c2-devkitm-1` 这块板，所以项目自带一份。

### `platformio.ini` 故意不锁平台版本

这台机器上装了好几套 `espressif32` 平台，并且**各自建一份 Python venv**，
状态不一样（其中一份 venv 坏了、缺 `pyvenv.cfg`）；另外还有一个 source 装的平台，
版本号 `53.03.10` 不是合法 semver，写进 `platform =` 会报
`SemanticVersionError: Invalid simple block`。

所以这里不锁版本，让 PlatformIO 用它自己的默认选择。**原因和理顺办法都写在
`platformio.ini` 文件头的注释里**，动这个文件之前先读那段。

### `sdkconfig` 是入库的

这不符合 ESP-IDF 例程的惯例（它们通常忽略它），但和 `ESP32S3_Ecam` 一个理由：
手工调过的选项一堆搬进 `sdkconfig.defaults` 更容易出错，入库保证换台机器 clone
下来能构建出一样的东西。

`sdkconfig.defaults` 里两条值得注意的：

- **`CONFIG_XTAL_FREQ_26=y`** —— 与参考项目 `c2_tracked_chassis` 的 C2 板配置一致，
  按参考项目的说明用于绕过该板子的时钟配置问题。更换硬件时要重新核对。
- **`CONFIG_PARTITION_TABLE_SINGLE_APP_LARGE=y`** —— 默认的 single-app 分区在 4MB
  flash 上只给 1MB，WiFi + httpd 可能顶到上限；这个给 1.5MB。不打算做 OTA，
  所以不需要双分区。

### flash 大小：这块模组是 2MB，板定义里写的是 4MB

**别信板定义里的 4MB。** 手上的 C2 模组是 2MB flash，而 `boards/` 里那份板定义
（抄的是官方 devkit）写的是 4MB，两边对不上会同时踩两个坑：

1. 每次构建都刷一条 `Flash memory size mismatch detected. Expected 4MB, found 2MB!`；
2. **构建最后一步 `elf2image` 直接失败**：
   `A fatal error occurred: Contents of segment at SHA256 digest offset 0xb0 are not all zero.`
   （PlatformIO 给 RISC-V 芯片硬编码了 `--elf-sha256-offset 0xb0`，flash 大小对不上
   时镜像布局变了，0xb0 就落不到那个空的 digest 字段上。）

修法是在 `platformio.ini` 里覆盖两条板级属性：

```ini
board_upload.flash_size = 2MB   ; 生成镜像和上传时用
board_build.flash_size  = 2MB   ; 让 IDF 那边的默认值也跟着对齐
```

改完 `sdkconfig.defaults` 要删掉 `sdkconfig.esp32-c2-devkitm-1` 再重建
（或者 `pio run -t fullclean`），否则不生效。

## 协议（UART1 配网 + UDP 驱动）

**纯文本行，同一套解析器跑在两个传输上**（`link_handle_line`）。UART 是
115200 8N1；UDP 是本机 `:3333` 上收单播。两边必须一致，对端的说明在
[Ecam 仓库 README](https://github.com/40chen/ESP32S3_Ecam) 的"外接模块 → 履带车"
一节，运动指令那一行和参考项目 `for_example/tank/c2_tracked_chassis` 一字兼容。

| 方向 | 行 | 走哪条 |
| --- | --- | --- |
| Ecam → C2 | `x<f> y<f>\n` | **两条都走**（没 IP 时 Ecam 走 UART） |
| Ecam → C2 | `#hi` | UART |
| C2 → Ecam | `#ok v1 tank` | UART，不答 = 旧固件 |
| Ecam → C2 | `#wifi 1 <b64 SSID>` / `#wifi 2 <b64 PASS>` / `#wifi 3` | **只走 UART** |
| C2 → Ecam | `#wifi ok` / `#wifi err <code>` | UART |
| C2 → Ecam | `#ip <addr>` | UART 在拿到 IP 时及收到 `#hi` 时报告；之后**每秒一条当 UDP 心跳** |

**串口脚是 C2 自己的编号，和 Ecam 那边是两套：** 本侧 TX=GPIO10、RX=GPIO18
（沿用参考项目）。Ecam 的 TX 接这里的 RX，Ecam 的 RX 接这里的 TX。

几处刻意的设计：

- **UDP 上不接受凭据**（`#wifi` 回 `#wifi err 4`）。这条不是防君子：运动指令
  无所谓（网页遥控本来就无鉴权，用户已经接受了），但**凭据不能让局域网里任何人
  写**——往 3333 端口打一行 `#wifi 1 ...` 就能改掉车连哪个网络，这个便宜不能给。
  所以 `link_handle_line()` 带一个 `allow_provisioning`，只有 UART 那条路传 true。
- **行式解析，攒到 `\n` 才解析。** 参考项目是每 20ms 读一次缓冲区直接 `sscanf`，
  凭据那种一百多字节的行一定会被切成两半。这里超过 160 字节的整行丢掉——
  **宁可丢一帧也不要拼半行**。
- **不认识的行必须无害。** `#` 开头的命令，新固件认识就处理，不认识只打条日志。
  这是双向兼容的地基：不然给新模块推凭据会变成让旧模块乱跑。
- **凭据分三行发、第三行才落盘。** 传到一半断了，不会在车上留下半套凭据。
  第三行之前没收到 SSID 会回 `#wifi err 3`。
- **payload 用 base64** 是为了凭据里的空格和中文 SSID 不会把"按空白分列"的协议
  撑破。**base64 是编码不是加密**，模块口上谁都能读——本地两根线可以接受，
  别以为它是安全的。
- **`#wifi ok` 只表示"凭据收下了"，不表示"连上了"。** 连没连上走 `#ip`；
  连不上则没有回报。Ecam 那边判的是"凭据送达"，不是"网络通了"。
- **UDP 明文，没有加密也没有认证。** 局域网里谁都可能往 3333 端口发包开车。
  和网页遥控的定位一样：车在用户脚边、局域网内可以接受，**别接到公共网络上**。

## 运动（motion.c）

两路 N20 电机，**驱动板上没有专用驱动 IC**：每路电机接两个 LEDC 通道
（A 正转、B 反转），靠占空比调速、靠哪一路有输出定方向。所以"负速度"不是负占空比，
是换一路。

引脚 GPIO4/5/6/7（M1=左轮、M2=右轮），4kHz / 13bit，沿用参考项目。
**模块板上的走线要对得上这四个脚**，对不上就改 `motion.c` 顶上那四个宏。

差速就是 `左 = y*100 + x*100`、`右 = y*100 - x*100`，两个都夹到 ±100。
相对参考实现改了两处：

1. **x/y 和左右速度两端都夹。** 参考实现只夹了上界，负的没夹，
   `-speed * 8192 / 100` 在 `speed < -100` 时会超 13 位满量程；
2. 去掉了 `m1/m2_coefficient` 系数（那是给遥控端微调用的，两个轮子同型号，用不上）。

### 看门狗归电机管，不归 UART

最后一次收到驱动指令起 **500ms** 没再来就停车。参考项目把这段放在 UART 解析里，
但那样只有 UART 一个来源——**现在有三条驱动路径**（Ecam 有线、Ecam 的 UDP
摇杆、手机网页），只看 UART 会把另外两条开着的时候每 500ms 停一次。

所以它归 `motion.c` 管：任何一条 `motion_drive()` 都把计时喂上，谁断了都停。
（Ecam 那边 20Hz 发一条、居中时也照发，所以"松手"和"断链"对它是一回事：
不发就是停，正好也是安全值。）

两个细节：

- **时间戳每次都喂，但只有非零指令才把"在动"标上。** 网页开着但没碰的时候一直在
  发 `0,0`，要是设了标志，看门狗每 500ms 就"停车"一次并打条警告，日志会刷屏。
- **用 32 位毫秒而不是 `esp_timer` 的 64 位微秒。** 这是两个任务之间共享的，
  32 位在 ESP32 上是单条指令读写（原子），64 位会有读到半个值的窗口——撕出来的
  那个数会让看门狗莫名其妙停一次车。毫秒的 32 位约 49 天回绕一次，而时间差用的是
  无符号减法，回绕本身也是对的。

## 网页遥控（web.c）

⚠️ **这个页面没有任何认证，谁连上同一个 WiFi 谁就能开这辆车。**
车在用户脚边、局域网内，可以接受；**别把它接到公共网络上。** 页面里那句提醒是认真的。

| 路径 | 作用 |
| --- | --- |
| `/` | 摇杆页面（几 KB 的内嵌单页） |
| `/drive?x=..&y=..` | 驱动，返回 `ok` |
| `/status` | 探针，返回 `{"ip":"...","connected":true}` |

**用 HTTP 轮询而不是 WebSocket。** 摇杆每 100ms 发一个 `GET /drive?x=..&y=..`，
板子这边就是一个无状态的 handler，不用管连接生命周期、不用处理异步发送失败。
局域网里一次请求往返个位数毫秒，100ms 足够跟手，而代码量是 WebSocket 那条路的
三分之一。（参考项目用的是 WebSocket + Vue，那是给带 PSRAM 的 S3 主机用的；
C2 只有 272KB RAM、4MB flash，那套搬不过来也不划算。）

**为什么是"定时轮询"而不是"只在动的时候发"：** 松开手要有一条明确的 `0,0`
发出去。定时轮询天然带这个行为，还给看门狗持续喂食。停了不发的话，得靠对面 500ms
超时停车，手感就是**"松手后还往前冲一下"**。

摇杆是 div + pointer 事件，不用 canvas（少一层坐标换算）；`lru_purge_enable`
是因为手机来去频繁，别把连接表占满。

## WiFi（wifi_prov.c）

凭据存在 NVS（namespace `tank`），**Ecam 推一次就够**，之后每次上电自己连——
车不能每次上电都等主机推一遍。

- **连上之后主动把 IP 报给 Ecam**（`#ip`），收到 `#hi` 也会重报当前状态：C2 没有屏幕，
  网页遥控的地址只有 Ecam 的履带车页能显示出来；重报可避免 Ecam 晚启动而错过 IP。
- **断开原因做了归类**：`auth`（密码错/握手超时）、`no ap`（找不到）、`disc <n>`。
  密码错和找不到 AP 是两回事，用户要能分清。
- **有凭据就一直重连，不做退避**——车就停在用户脚边，断线重连越早越好。
- **换网络要先 `esp_wifi_disconnect()` 再连**，否则 `esp_wifi_connect()` 可能直接
  返回 "already connected" 而什么都不做。
- 超长的 SSID / 密码**不截断，直接报错**，让上层明确失败。

## 已知问题

- **`#wifi ok` 报早了。** 它表示"凭据已收下"，真正连上没有回报。想让它反映
  "连上了"得等 `WIFI_EVENT_STA_DISCONNECTED` 里的 `s_err` 有个回传通道，
  目前 `wifi_prov_last_error()` 写好了但**没人调用**。
- **网页和 UDP 都无认证**，见上。
- **`motion.c` 的引脚是照参考项目抄的**，没有在实物上核对过模块板的走线。
- `include/` `lib/` `test/` 是 PlatformIO 脚手架留下的空目录，可以删。

## 和相关仓库的关系

运动控制移植自 `for_example/tank/c2_tracked_chassis`；协议和 `ESP32S3_Ecam`
的"外接模块 → 履带车"一节是**一份契约的两半**，改一边必须改另一边。

这个仓库是**车的那一半**，主机那一半（摇杆 UI、二维码、UDP 发包节拍）在
[40chen/ESP32S3_Ecam](https://github.com/40chen/ESP32S3_Ecam)。两边一起改的时候
最容易漏的是**行格式**和**端口号 3333**（本仓库 `src/udp.h` 的 `UDP_PORT`，
对面 `main/app/app_tank.c` 的 `APP_TANK_UDP_PORT`）。
