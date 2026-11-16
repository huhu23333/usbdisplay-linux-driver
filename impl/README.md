# usbdisplay 用户态驱动实现

本目录包含 C++17 用户态驱动源码、工具与测试。构建方法、投屏方法与协议说明见仓库根目录 README.md 以及 docs 目录。

## 构建

```bash
cmake -S impl -B build
cmake --build build -j
```

## 测试

```bash
ctest --test-dir build
```

测试覆盖访问单元拆分与起始码保留、控制通道报告封装、视频 URB 封装、SPS 帧率解析、音频输入解析以及自动复位状态机。

## 目录结构

```
impl/include/usbdisplay/   公开接口
impl/src/common/           日志、JSON 与通用工具
impl/src/proto/            Annex-B 拆分、SPS 解析、WAV 读取与 IP 通道编解码
impl/src/hid/              控制通道报告封装与转储
impl/src/control/          JSON-RPC 与 HID 控制通道
impl/src/transport/        hidraw、libusb 视频通道、设备复位与网络传输
impl/src/audio/            ALSA 播放
impl/src/controller.cpp    会话控制与自动复位
impl/tools/                诊断、发送、探测、JSON 会话、音频与模拟接收器
impl/tests/                单元测试与跨进程测试
```

## 通道说明

控制通道使用 HID 接口 0，报告编号 1，报告长度 4096 字节，承载 JSON-RPC 消息。
视频通道使用 HID 接口 1 的端点 2 输出方向，由 libusb 发送 4096 字节 URB。
音频通道 auto 模式按环境选择：PulseAudio 能看到 USB 声卡时使用显式 USB sink，不修改 card profile 和默认 sink；看不到时使用 ALSA 直写 plughw:USBDISPLAY,0。ALSA 打开失败按 200ms 间隔重试，最长 5 秒。若选择 ALSA 后仍被 PulseAudio 占用，临时把 card profile 设为 off，退出、异常、auto-reset 前恢复，恢复失败记录警告。PulseAudio 后端使用 pa_stream，显式指定 USB sink 并设置 PA_STREAM_DONT_MOVE，默认 sink 变化时流不移动；实现忽略 moved 事件，不修改默认 sink。默认 sink 只读记录。音频与视频可单独配置：未配置视频时只做控制握手与音频，未配置音频时只推视频；音频打开成功后才发送首帧视频，一侧运行时失败不影响另一侧。
会话恢复通过 libusb_reset_device 使设备重新枚举，然后重新执行握手。
