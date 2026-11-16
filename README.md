# USB Display Linux 用户态驱动

适用于 **SHUOMENG SM-A2H5S**（USB 转 HDMI 转接器，USB ID `1de1:f201`，设备名 `USB_DISPLAY`）。

纯用户态实现：**不修改内核、不改系统文件**；通过 hidraw 发送 JSON-RPC 控制协议，通过 libusb 直接向 EP2 发送 4096 字节 URB 视频帧。

## 已验证状态

- 1920x1080@60fps / 约 4Mbps：**流畅投屏**（H.264 硬件编码 VAAPI/QSV 与 x264 软编均可）
- 连续两次运行：**无需拔插、无需 sudo**（udev 规则 + 程序内 `libusb_reset_device` 自动复位重连）
- `ctest`：14/14 通过；构建 0 警告
- 音频：设备为标准 USB Audio，可用 ALSA 与视频同时播放

## 目录结构

```
impl/                 C++17 用户态驱动（libusb 视频 + hidraw 控制 + 自动复位）
tools/install.sh      一次性安装脚本（udev 权限，sudo 运行一次）
tools/usbdisplay_encode.sh  任意视频 -> 设备兼容 H.264（默认 x264，可选 --vaapi）
tools/usbdisplay_run.sh     投屏封装脚本（自动设置 iHD 环境）
tools/99-usbdisplay.rules   udev 规则
docs/                 协议与使用文档副本
```

## 依赖

- `cmake`、`g++`、`libusb-1.0-0-dev`、`pkg-config`
- `ffmpeg`（编码/转码，可选）
- ALSA（音频，可选）
- Intel 核显硬编需 `intel-media-va-driver`（Meteor Lake 等新核显需 >= 24，源码编译方法见上级目录 `work/va2/`）

## 安装（一次性，需要 sudo）

```bash
cd usbdisplay-linux-driver
sudo tools/install.sh      # 写入 udev 规则并 reload/trigger
```

之后普通用户直接使用，无需 sudo/chmod，也无需手动拔插设备。卸载：`sudo tools/install.sh --uninstall`。

## 构建与测试

```bash
cmake -S impl -B build && cmake --build build -j
ctest --test-dir build      # 14/14
```

## 编码（任意机器）

设备要求 H.264 码流特征（实测结论）：

| 项 | 要求 |
|---|---|
| 封装 | Annex-B，每帧以 AUD 开头，保留 `00 00 00 01` 起始码 |
| Profile/Level | Main (77) / <=4.2 |
| 熵编码 | CABAC |
| B 帧 | 禁止（`bframes=0`） |
| 参考帧 | `ref=1`，无重排序（max_num_reorder_frames=0） |
| 分辨率/帧率 | 1920x1080 / 60fps |
| 码率 | 3.5–4.5 Mbps，帧大小尽量均匀 |

用 x264（可移植，任何 Linux/Windows 机器均可）：

```bash
ffmpeg -y -i input.mp4 -c:v libx264 -profile:v main -level:v 4.2 \
  -pix_fmt yuv420p -b:v 4M -maxrate 4200k -bufsize 4000k -r 60 \
  -x264-params "aud=1:repeat-headers=1:bframes=0:ref=1:keyint=120:scenecut=0" \
  -f h264 stream.h264
```

或使用脚本：

```bash
tools/usbdisplay_encode.sh input.mp4 stream.h264 60 4M          # x264
tools/usbdisplay_encode.sh input.mp4 stream.h264 60 4M --vaapi  # Intel 核显
```

> 不满足上表时：起始码缺失 → 黑屏；带 B 帧 → 能显示但周期性抽动。

## 投屏

```bash
./build/usbdisplay_send --live --h264 stream.h264
tools/usbdisplay_run.sh stream.h264                 # 封装脚本
./build/usbdisplay_send --live --h264 stream.h264 --wav audio.wav   # 带音频
```

帧率自动从 SPS VUI 读取；默认完整握手并等待 `isAllowed`；失败自动复位重试
（`--auto-reset` 默认开，`--reset-retries`/`--reset-wait-ms` 可调）。

## 工作原理（简述）

- 控制：HID IF0（report ID 1，4096B），JSON-RPC 2.0；心跳必须带 session-id；
  8 步启动序列后设备回 `{"result":"isAllowed"}`。
- 视频：HID IF1 / EP2 OUT，4096 字节 URB；首包 16B 头
  `[0]=2 [1..4]=BE32 帧长 [5..7]=0 [8..11]=宽 [12..15]=高 [16..]=H.264`，
  续包 16B 全零头。
- 会话恢复：`libusb_reset_device` 让设备重新枚举（等效拔插），再重新发现节点并握手。

详细协议与全部实测证据见 [docs/USB_PROTOCOL_VERIFIED.md](docs/USB_PROTOCOL_VERIFIED.md)；
完整使用说明见 [docs/FINAL_USAGE.md](docs/FINAL_USAGE.md)。

## 已知限制

- 视频/音频走不同 USB 通道，无设备级 A/V 同步协议（各自按时间播放）。
- 设备固件只接受新枚举后的会话（已由自动复位处理）。
- 输出分辨率由设备 EDID 固定为 1920x1080。
