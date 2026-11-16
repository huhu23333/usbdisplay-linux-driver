// SPDX-License-Identifier: MIT
#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace usbdisplay {

// 接收器默认 USB 标识（控制通道所在设备）。
constexpr uint16_t kDefaultVid = 0x1DE1;
constexpr uint16_t kDefaultPid = 0xF201;
// 网络传输模式的默认地址。USB 模式不使用该地址。
constexpr const char* kDefaultDeviceHost = "192.168.203.1";

enum class Codec : uint8_t { H264 = 0, HEVC = 1 };

// 早期 HID 报告路径的视频封装模式，当前 USB 视频通道不使用。
enum class FrameMode : uint8_t {
  AppRaw = 0,
  AppInner = 1,
  Ugreen = 2,
};

// 视频流参数。
struct VideoConfig {
  Codec codec = Codec::H264;
  uint16_t width = 1920;
  uint16_t height = 1080;
  uint16_t fps = 60;
  uint32_t bitrate_bps = 8'000'000;
  bool fullscreen = false;
};

enum class AudioFormat : uint8_t {
  PCM_S16LE = 0,  // 交织排列的 16 位有符号小端采样
  PCM_F32LE = 1,  // 交织排列的 32 位浮点小端采样
};

struct AudioConfig {
  uint32_t sample_rate = 48000;
  uint16_t channels = 2;
  AudioFormat format = AudioFormat::PCM_S16LE;
  // PCM 音频分组时长。
  uint16_t frame_ms = 20;
};

enum class TraceLevel : int {
  Off = 0,
  Error = 1,
  Warn = 2,
  Info = 3,
  Debug = 4,
  Wire = 5,  // 输出每个报文的十六进制内容
};

enum class LinkMode : uint8_t {
  Auto = 0,             // HID control if present + IP media channel
  IpOnly = 1,           // no HID control, media over TCP/UDP only
  UsbOnly = 2,          // HID control only (diagnostic / handshake only)
};

// 媒体传输方式。
enum class MediaTransport : uint8_t {
  Tcp = 0,
  Udp = 1,
  Loopback = 2,  // 进程内回环，供测试与离线演练使用
  Null = 3,      // 只统计帧数与字节数，不发送数据
  UsbHid = 4,    // USB 通道：控制通道使用 HID 接口零，视频通道使用 libusb 端点二
};

// 设备定位参数。
struct DeviceSelector {
  LinkMode mode = LinkMode::Auto;
  uint16_t vid = kDefaultVid;
  uint16_t pid = kDefaultPid;
  // 控制通道的 hidraw 节点路径，留空时按 VID/PID 自动发现。
  std::string hidraw_path;
  // IP 网络传输路径的接收端地址，USB 模式不使用。
  std::string host = kDefaultDeviceHost;
  // IP 网络传输路径的本地绑定地址，留空表示由系统选择。
  std::string bind_address;
  // 取值为零表示使用默认端口。
  uint16_t control_port = 0;
  uint16_t video_port = 0;
  uint16_t audio_port = 0;
  MediaTransport media_transport = MediaTransport::Tcp;
  bool use_udp_audio = true;
  // IP 网络传输路径的封装格式，可选 legacy-v1 或 rtp。
  std::string wire_format = "legacy-v1";
  // 每个媒体报文的最大负载字节数，UDP 传输时需小于路径 MTU。
  size_t max_payload = 1400;
  int control_timeout_ms = 3000;
  int connect_timeout_ms = 3000;

  // USB 模式参数。
  // 控制通道节点留空时按 USB 接口编号自动发现，IF0 为控制通道。
  std::string control_hidraw_path;
  // 视频通道使用 libusb，不经过 hidraw，该字段仅为兼容保留。
  std::string video_hidraw_path;
  // 控制通道的 HID 报告编号与报告长度，需与设备报告描述符一致。
  uint8_t control_report_id = 1;
  size_t control_report_size = 4096;
  // 视频通道的报告编号与 URB 长度，默认使用 4096 字节。
  uint8_t video_report_id = 2;
  size_t video_report_size = 4096;
  // 每个报告都携带外层头部，供控制通道与历史测试使用。
  bool report_header_per_chunk = false;
  // 以下字段属于早期网络传输构建逻辑，当前 USB 视频通道不使用。
  uint32_t video_msg_type = 0;
  uint32_t video_field_1 = 1;
  uint32_t video_field_a = 0;
  uint32_t video_field_b = 0;
  uint32_t video_field_c = 0;
  bool video_magic_1000 = true;
  size_t video_chunk_payload = 0;
  FrameMode frame_mode = FrameMode::AppRaw;
  bool video_inner_payload_len = false;
  uint8_t ugreen_type = 0;
  uint16_t ugreen_flags = 0;
  uint32_t ugreen_fraginfo = 0;
  std::string ugreen_magic = "RRIM";
  uint32_t video_outer_param3 = 0;
  uint32_t video_outer_param4 = 0;
  // 兼容保留字段，当前未使用。
  bool require_video_hidraw = false;
  // 是否跳过控制通道，仅发送视频。
  bool disable_control = false;
  // 非空时把生成的报文写入转储文件，供离线测试使用。
  std::string report_dump_path;
  std::string control_report_dump_path;
  // 视频端点的 libusb 传输类型，可选 auto、interrupt、bulk。
  std::string usb_transfer = "auto";
  // 握手失败时自动复位设备并重新枚举，最多重试三次。
  bool auto_reset = true;
  int auto_reset_retries = 3;
  int auto_reset_wait_ms = 5000;
  // 退出时是否复位设备，默认关闭。
  bool reset_on_exit = false;
  // USB 音频接口对应的 ALSA 设备，留空表示不播放音频。
  std::string alsa_device = "plughw:USBDISPLAY,0";
  // 音频输出后端：
  // auto 根据 PulseAudio 是否列出 USB 声卡选择后端；
  // alsa 直写 plughw:USBDISPLAY,0（安装音频隔离规则时使用）；
  // pulse 使用显式 USB sink（未安装音频隔离规则时使用）。
  std::string audio_backend = "auto";
  // PulseAudio sink 名称，留空时自动查找 USB 设备对应的 sink。
  std::string pulse_sink;
  // 显式允许恢复启动时保存的默认 sink。默认只读记录，不修改系统设置。
  bool restore_default_sink = false;
  // 为真时不打开视频通道，仅做控制握手与音频推送，用于音频单独投屏。
  bool disable_video = false;

  // 早期控制通道参数，当前代码不使用。
  uint16_t hid_report_size = 64;
  uint8_t hid_report_id = 0;
  bool hid_feature_report = false;
  bool hid_chunked = false;

  // IP 网络传输路径启动时是否发送流配置报文。
  bool send_stream_config = true;
  // 握手失败时 start() 是否返回失败。
  bool require_handshake = false;
  // 是否在 start() 中执行设备握手。
  bool send_handshake = false;
  // 是否播放 USB 音频。音频接口与视频接口相互独立，可只启用一侧。
  bool enable_audio = true;
  // 音频失败时是否让 start 失败；为假时音频失败只记录日志，视频继续。
  bool require_audio = false;

  static DeviceSelector usb(uint16_t vid = kDefaultVid, uint16_t pid = kDefaultPid);
  static DeviceSelector network(const std::string& host);
};

// 累计统计值，通过 Controller::stats() 获取快照。
struct Stats {
  uint64_t video_frames = 0;
  uint64_t video_bytes = 0;
  uint64_t video_packets = 0;
  uint64_t video_keyframes = 0;
  uint64_t audio_frames = 0;
  uint64_t audio_bytes = 0;
  uint64_t audio_packets = 0;
  uint64_t control_tx = 0;
  uint64_t control_rx = 0;
  uint64_t send_errors = 0;
  uint64_t dropped_packets = 0;   // 发送端背压导致的丢包
  uint64_t heartbeat_tx = 0;
  uint64_t heartbeat_failures = 0;
  uint64_t last_video_pts_us = 0;
  uint64_t last_audio_pts_us = 0;
  uint64_t uptime_ms = 0;
};

enum class EventType : uint8_t {
  DeviceFound,
  Opened,
  Configured,
  StreamingStarted,
  StreamingStopped,
  KeyframeRequested,   // 调用方应把下一帧编码为 IDR
  ControlMessage,      // 收到设备发来的 JSON-RPC 消息
  WirePacket,
  Error,
};

struct Event {
  EventType type = EventType::Error;
  std::string message;   // 可读说明
  std::string method;    // 控制消息的 JSON-RPC 方法名
  std::string json;      // 控制消息的原始参数或结果
  int code = 0;          // 可选错误编号
};

using EventCallback = std::function<void(const Event&)>;
using TraceSink = std::function<void(TraceLevel, const std::string&)>;

}  // namespace usbdisplay
