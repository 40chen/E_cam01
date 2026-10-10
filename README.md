# E_cam 智能相机与控制终端

基于 ESP32-S3 的智能相机与控制终端项目，包含相机、音乐播放、TF 卡管理、外接 ESP32-C2 履带车控制，以及用于 API 用量和电脑硬件状态监控的 Windows 上位机。

本仓库将主机固件、履带车固件和上位机集中管理；各模块仍使用各自的工具链独立构建。

## 项目组成

| 目录 | 模块 | 主要功能 | 详细说明 |
| --- | --- | --- | --- |
| [`ESP32S3_Ecam/`](ESP32S3_Ecam/) | ESP32-S3 主机固件 | 相机取景与拍照、照片浏览、音乐播放、Wi-Fi 与邮箱配置、TF 卡 USB 读卡器模式、外接模块及状态副屏 | [`README.md`](ESP32S3_Ecam/README.md) |
| [`ESP32C2_tank/`](ESP32C2_tank/) | ESP32-C2 履带车固件 | 双电机履带底盘控制、Wi-Fi 配网、UDP 遥控、手机网页遥控和失联停车保护 | [`README.md`](ESP32C2_tank/README.md) |
| [`Ecam_display/`](Ecam_display/) | Windows 上位机 | API 余额与 Tokens 用量估算、CPU/内存/GPU/网速监控，并通过串口向 ESP32-S3 副屏发送状态 | [`README.md`](Ecam_display/README.md) |

## 模块协作

```text
Windows 上位机 ── UART0：电脑状态 JSON ──→ ESP32-S3 主机
                                             │
                                             ├── UART1：Wi-Fi 配网
                                             └── UDP :3333：履带车遥控
                                                        ↓
                                                 ESP32-C2 履带车
```

- ESP32-S3 主机负责相机、触摸屏界面和外接模块入口。
- ESP32-C2 履带车保存 Wi-Fi 凭据后，通过 UDP 接收主机的运动指令；UART1 用于配网和状态交互。
- Windows 上位机通过串口将电脑状态发送给 ESP32-S3 的副屏功能。
- 主机与履带车之间的串口消息格式、UDP 端口和控制行为必须保持一致。修改通信协议时，请同时检查两侧固件及各自 README 中的协议说明。

## 构建与运行

### ESP32-S3 主机固件

需要 ESP-IDF 5.1 或更新版本，并先按 ESP-IDF 文档配置好开发环境。

```powershell
cd ESP32S3_Ecam
idf.py set-target esp32s3
idf.py build
idf.py -p COMx flash monitor
```

将 `COMx` 替换为开发板实际使用的串口。目标芯片已配置过时，可跳过 `set-target`。

### ESP32-C2 履带车固件

需要 PlatformIO Core 及项目所需的 ESP-IDF 环境：

```powershell
cd ESP32C2_tank
pio run
pio run -t upload
pio device monitor
```

该项目的串口监视器波特率为 74880；详细板卡配置和通信协议请参阅 [履带车固件说明](ESP32C2_tank/README.md)。

### Windows 上位机

需要 Python 3.8 或更新版本。安装依赖并启动：

```powershell
cd Ecam_display
pip install requests psutil pyserial pynvml keyring
python Ecam_Status_Display.py
```

其中 GPU 监控依赖 NVIDIA 显卡及驱动；详细配置、打包和安全存储说明请参阅 [上位机说明](Ecam_display/README.md)。

## 安全提示

履带车的 UDP 控制和手机网页遥控目前没有认证或加密。请仅在可信的本地网络中使用，不要将履带车控制服务暴露到公共网络。
