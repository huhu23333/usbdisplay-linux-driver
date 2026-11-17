# USB Display Linux 用户态驱动

适用设备：SHUOMENG SM-A2H5S USB 转 HDMI 转接器，USB ID 1de1:f201，设备名称 USB_DISPLAY。

本仓库提供该设备的 Linux 用户态驱动实现，包含控制通道、视频通道、会话恢复与配套工具。

## 已验证能力

- evdi 独立虚拟显示服务：单个 EDID 同时暴露 1920x1080@60 和 @120，KDE 显示设置可直接切换刷新率；USB 发送固定 60fps、约 4 Mbps 码率。
- 开放控制接口 `usbdisplay_evdi_ctl connect|disconnect|status`，不依赖 USB 驱动；USB 驱动仅在插拔时调用，抓帧服务完全独立。
- 连续两次运行之间不需要拔插设备，不需要 sudo 权限（安装脚本完成权限配置后）。
- 构建无警告，单元测试与集成测试共 30 项全部通过。
- 音频与视频可分别投屏：音频后端 auto 按安装模式选择，安装了音频隔离规则时 ALSA 直写，未安装时使用 PulseAudio 显式 USB sink；程序音频只送到设备，视频侧不依赖音频。

## 目录结构

```
impl/                         C++17 用户态驱动
tools/install-video-rules.sh  仅安装 USB/HID 视频权限规则
tools/install-audio-isolation.sh  仅安装音频隔离规则
tools/evdi-precreate.sh       evdi 专用虚拟显示设备预创建（root）
tools/usbdisplay-evdi.service evdi grabber systemd 服务（ExecStartPre 预创建 + ExecStart 常驻 grabber）
tools/install-evdi-support.sh evdi grabber/unit/组/Xorg 安装卸载脚本
tools/usbdisplay-daemon.service 消费 daemon systemd user 服务
install.sh                    一键安装视频规则、evdi 支持、grabber、daemon 与用户服务
impl/tools/usbdisplay_evdi_grabber.cpp  只抓帧并写共享内存，不编码/USB
impl/tools/usbdisplay_daemon.cpp        只读共享帧、x264 编码与 Controller 推送
impl/src/evdi/shared_frame.*            三槽共享帧协议
impl/tools/usbdisplay_evdi_ctl.cpp      控制 CLI（connect/disconnect/status）
impl/third_party/evdi/        libevdi 头文件（LGPL-2.1-or-later）
tools/usbdisplay_encode.sh    视频转码脚本
tools/usbdisplay_run.sh       投屏脚本
tools/99-usbdisplay-usb.rules  USB/HID 权限规则
tools/99-usbdisplay-audio-ignore.rules  可选音频隔离规则
docs/                         协议文档与使用说明副本
docs/EVDI_SETUP.md            evdi 模块、专用设备与 X11/Wayland 集成说明
```

## 依赖

- cmake、g++、pkg-config
- libusb-1.0 开发包
- ffmpeg（转码时使用）
- ALSA（音频直写与回退）
- libpulse（PulseAudio 播放流与声卡 profile 临时释放时使用）
- libevdi（可选 grabber）、libx264/libswscale（可选消费 daemon）；缺失时可用 -DUSBDISPLAY_BUILD_DAEMON=OFF 关闭构建
- Intel 核显硬编需要 intel-media-va-driver，Meteor Lake 等新核显需要 24 及以上版本

## 安装

两个安装脚本相互独立，可以单独安装或卸载：

```bash
sudo tools/install-video-rules.sh          # 仅 USB/HID 视频权限规则
sudo tools/install-video-rules.sh --uninstall
sudo tools/install-audio-isolation.sh      # 仅音频隔离规则
sudo tools/install-audio-isolation.sh --uninstall
```

未安装音频隔离规则时，PulseAudio 会把该设备当作普通桌面音频输出，auto 模式使用 PulseAudio 显式 USB sink，用户可在 KDE 音频设置中选择设备，程序不修改默认输出。安装音频隔离规则后需要重新插拔设备，PulseAudio 不再看到该 USB 声卡，auto 模式改用 ALSA 直写 plughw:USBDISPLAY,0，桌面音频与 KDE 界面保持原设备。两个脚本分别管理视频权限与音频隔离，卸载其中一个不影响另一个。

evdi 虚拟显示支持独立安装，不影响上述视频/音频规则：

```bash
sudo tools/install-evdi-support.sh
sudo tools/install-evdi-support.sh --uninstall
```

安装脚本会安装 `usbdisplay-evdi.service`：`ExecStartPre` 在显示管理器之前创建/复用
专用 1920x1080 evdi card，`ExecStart` 常驻 `usbdisplay_evdi_grabber`，两者同生命周期，
grabber 是独立 evdi 虚拟显示服务，默认 disconnected；通过 `/run/usbdisplay/control.sock`
（0660、组 `usbdisplay`）接受 CONNECT/DISCONNECT/STATUS，任何程序可用
`usbdisplay_evdi_ctl connect [60|120]|disconnect|status` 控制，不依赖 USB；虚拟显示
支持 1080p60/120（单个 EDID 同时暴露两个模式，桌面可直接切刷新率），USB
发送固定 60fps。USB 驱动在 USB
在线/离线时调用同一接口，自身只取帧投屏。

grabber 只抓帧并写入 `/dev/shm/usbdisplay-frames`（三槽、0660、
组 `usbdisplay`），并在 `/run/usbdisplay/frames.sock` 提供 ACQUIRE/RELEASE 租约；不编码、
不访问 USB 视频接口、不等待 consumer。consumer 只读映射共享帧，绝不写槽状态。
USB 在线时才 `evdi_connect2` 提交含 `USBDisplay` 名称的 EDID，拔出即断开。消费 daemon
不直接访问 evdi；socket 断开/共享 inode 变化/旧 magic 时关闭并重开。本机已有 evdi 设备不会被
枚举、修改或删除，evdi 路径不使用音频。权限、marker 所有权与清理限制见
[docs/EVDI_SETUP.md](docs/EVDI_SETUP.md)。

### 一键安装扩展示显示屏（可选）

手动视频输出与扩展示显示屏是两层功能：

- 手动视频输出：安装 `tools/install-video-rules.sh` 后，用 `usbdisplay_send` 或 C++ Controller 手动发送 H.264。
- 扩展示显示屏：单个 systemd 服务先预创建 evdi card，再启动常驻 grabber；grabber 只抓帧写共享内存，USB 插入时才连接 evdi，拔出即断开。消费 daemon 只读共享帧并编码推送，不直接访问 evdi；USB 在线才启动 Controller，失败不影响 grabber。

```bash
sudo ./install.sh
sudo ./install.sh --uninstall
```

一键脚本会安装 USB/HID 视频权限规则，构建并安装 `usbdisplay_evdi_grabber` 与
`usbdisplay_daemon`，安装 evdi 服务（含预创建、usbdisplay 组、Xorg 配置），再把
消费 daemon 的 user unit 安装到当前 sudo 用户。安装时不会提前启动用户 daemon；
登录桌面后执行 `systemctl --user start usbdisplay-daemon.service`。它不安装音频隔离
规则，也不修改音频配置。若用户 systemd 会话不可用，脚本会打印手动 enable/start 命令。
共享帧与 socket 权限需要重新登录使 usbdisplay 组生效；架构、失败恢复、KWin 多 GPU 与卸载限制见
[docs/EVDI_SETUP.md](docs/EVDI_SETUP.md)。

## 构建与测试

```bash
cmake -S impl -B build
cmake --build build -j
ctest --test-dir build
```

evdi grabber/daemon 是可选目标；缺少 libevdi、libx264 或 libswscale 时可执行
`cmake -S impl -B build -DUSBDISPLAY_BUILD_DAEMON=OFF` 关闭。消费 daemon 不链接
libevdi；grabber 不链接 libx264/libswscale。

## 编码

设备兼容的 H.264 码流要求：

- 封装：Annex-B 裸流，每个访问单元以 AUD 起始，保留 00 00 00 01 起始码。
- Profile 与 Level：Main (77)，不高于 4.2。
- 熵编码：CABAC。
- B 帧：禁止。
- 参考帧数量：1，无重排序，解码帧缓冲数量为 1。
- 分辨率与帧率：1920x1080，60 帧每秒。
- 码率：3.5 至 4.5 Mbps，帧大小尽量均匀。

软件编码命令（任何机器均可运行）：

```bash
ffmpeg -y -i input.mp4 -c:v libx264 -profile:v main -level:v 4.2 \
  -pix_fmt yuv420p -b:v 4M -maxrate 4200k -bufsize 4000k -r 60 \
  -x264-params "aud=1:repeat-headers=1:bframes=0:ref=1:keyint=120:scenecut=0" \
  -f h264 stream.h264
```

转码脚本：

```bash
tools/usbdisplay_encode.sh input.mp4 stream.h264 60 4M
tools/usbdisplay_encode.sh input.mp4 stream.h264 60 4M --vaapi
```

## 投屏

```bash
# 音视频同时投屏
./build/usbdisplay_send --live --h264 stream.h264 --wav audio.wav
# 仅视频（不播放 WAV）
./build/usbdisplay_send --live --h264 stream.h264 --no-audio
# 仅音频（控制握手 + 音频，不发送视频帧）
./build/usbdisplay_send --live --wav audio.wav --no-video
# 单独的音频工具，使用同一 Controller 会话完成握手后只推音频
./build/usbdisplay_audio --wav audio.wav
./build/usbdisplay_audio --tone-hz 440 --seconds 3
# 显式覆盖音频后端（auto 为默认）
./build/usbdisplay_send --live --h264 stream.h264 --wav audio.wav --audio-backend pulse
```

帧率从 SPS 的 VUI 参数读取。程序默认执行完整握手并等待设备返回 isAllowed，握手失败时自动复位设备并重试。仅音频模式会打印一行提醒：设备在没有视频输入时可能不输出音频。

## 工作原理

- 控制通道：HID 接口 0，报告编号 1，报告长度 4096 字节，传输 JSON-RPC 2.0 消息。心跳消息携带会话编号，设备回复相同编号。
- 视频通道：HID 接口 1 的端点 2 输出方向，通过 libusb 发送 4096 字节 URB。首包包含 16 字节头部：报告编号 2、大端序帧长度、3 字节保留、大端序宽度、大端序高度；之后为 H.264 数据。续包头部 16 字节全部为零。
- 音频通道：auto 模式查询 PulseAudio 声卡列表。能看到 USB 声卡时使用 PulseAudio 显式 USB sink，不修改 card profile 和默认 sink；看不到时使用 ALSA 直写 plughw:USBDISPLAY,0。ALSA 打开失败按 200ms 间隔重试，最长 5 秒。--audio-backend alsa|pulse 可显式覆盖。
- 声卡回退：auto 选择 ALSA 后若仍被 PulseAudio 占用，说明音频隔离规则未生效或未重新插拔，程序会临时把该声卡 profile 设为 off 释放设备，并在退出、异常、auto-reset 前恢复原 profile；恢复失败记录警告，同时提示执行 sudo tools/install-audio-isolation.sh 并重新插拔。
- 音视频解耦：未配置音频时视频照常工作；未配置视频时 Controller 只做控制握手与音频。音频打开成功后才发送首帧视频，首帧音频与首帧视频同时开始，音频启动时间戳写入日志。运行时一侧推送失败只记录日志，另一侧继续。
- PulseAudio 后端使用 pa_stream，连接时显式指定 USB sink 并带上 PA_STREAM_DONT_MOVE，目标缓冲延迟 100ms 级别。系统默认输出切换只影响桌面音频，程序播放流不会被移动；实现忽略 moved 事件，不主动移动流，也不修改默认 sink。auto 在未安装音频隔离规则时会选择该后端。
- 默认 sink 只读记录：启动时读取并记录，不调用 set_default_sink；--restore-default-sink 仅在显式开启时允许恢复。
- 会话恢复：调用 libusb_reset_device 使设备重新枚举，再重新发现设备节点并执行握手，作用与手动拔插相同。

协议细节与实测记录见 [docs/USB_PROTOCOL_VERIFIED.md](docs/USB_PROTOCOL_VERIFIED.md)，使用说明见 [docs/FINAL_USAGE.md](docs/FINAL_USAGE.md)。

## 已知限制

- 视频与音频使用不同的 USB 通道，没有设备级音视频同步协议。
- 未安装音频隔离规则时，PulseAudio 会把该设备当作普通音频输出，KDE 音频设置界面会显示该外接设备，用户可以自行选择；安装音频隔离规则并重新插拔后，PulseAudio 不再看到该声卡，桌面音频与 KDE 界面保持原设备。
- 仅音频模式下设备可能要求先有视频输入才输出声音，程序只打印提醒，不做额外限制。
- 设备固件只接受重新枚举后的会话，程序通过自动复位处理。
- 输出分辨率由设备 EDID 固定为 1920x1080。
