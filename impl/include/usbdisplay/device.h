// SPDX-License-Identifier: MIT
// 控制器公开接口。
//
// 调用方负责视频采集与 H.264 编码，本库负责分组与发送。
// 视频通道与音频通道相互独立，可以只启用一侧：
//   只视频：configure(video, zero_audio)，start 后只调用 push_video；
//   只音频：configure(zero_video, audio)，start 后只调用 push_audio，
//           无视频输入时设备可能不输出声音；
//   同时：configure(video, audio)，分别调用 push_video 与 push_audio。
// 音频后端由 DeviceSelector::audio_backend 选择：auto 根据 PulseAudio 是否
// 列出 USB 声卡自动选择，alsa 直写 plughw:USBDISPLAY,0，pulse 使用显式
// USB sink。两种音频接口不依赖对方，视频写入与音频播放不共享缓冲或互斥。
// 典型用法：
//   usbdisplay::Controller ctl;
//   std::string err;
//   auto sel = usbdisplay::DeviceSelector::usb();
//   if (!ctl.open(sel, &err)) fail(err);
//   usbdisplay::VideoConfig v;  // 1920x1080，60 帧每秒，8 Mbps
//   usbdisplay::AudioConfig a;  // 48 kHz，双声道，16 位有符号小端
//   if (!ctl.configure(v, a, &err)) fail(err);
//   ctl.start(&err);
//   ctl.push_video(annexb_data, annexb_len, pts_us, /*keyframe=*/true);
//   ctl.push_audio(pcm, pcm_bytes, pts_us);
//   ctl.close();
//
// 所有方法均可多线程调用。发送方法在内部超时时间内阻塞，
// 传输失败时返回假并写入错误信息。
#pragma once

#include <cstdint>
#include <memory>
#include <string>

#include "usbdisplay/types.h"

namespace usbdisplay {

class Controller {
 public:
  Controller();
  ~Controller();
  Controller(const Controller&) = delete;
  Controller& operator=(const Controller&) = delete;

  // 查找并打开接收器。自动模式下解析 HID 控制通道与媒体通道；
  // 两者均不可用时返回 false，除非配置了回环或空传输。
  bool open(const DeviceSelector& selector, std::string* err = nullptr);
  void close();
  bool is_open() const;

  // 与接收器协商流参数。仅使用控制通道；没有控制通道时只记录配置，
  // 调用方需要保证接收器已进入投屏状态。
  bool configure(const VideoConfig& video, const AudioConfig& audio,
                 std::string* err = nullptr);
  bool start(std::string* err = nullptr);
  bool stop();

  // 仅停止音频播放，视频推送继续进行；随后可调用 start_audio 恢复。
  bool stop_audio();
  bool start_audio(std::string* err = nullptr);

  // 发送一个 Annex-B 形式的访问单元。起始码原样保留，分组器只做传输分片。
  bool push_video(const uint8_t* annexb, size_t len, uint64_t pts_us,
                  bool keyframe, std::string* err = nullptr);

  // 发送 PCM 音频，采样格式需与配置一致。数据按 frame_ms 分片，
  // pts_us 为本缓冲区首字节的显示时间。
  bool push_audio(const void* pcm, size_t bytes, uint64_t pts_us,
                  std::string* err = nullptr);

  // 请求接收器丢弃数据直到下一个 IDR，同时触发 KeyframeRequested 事件，
  // 调用方据此强制编码器输出关键帧。没有控制通道时返回假。
  bool request_keyframe(std::string* err = nullptr);

  // 通过控制通道发送 JSON-RPC 请求并返回原始 JSON 响应。
  bool send_control(const std::string& method, const std::string& params_json,
                    std::string* response_json, std::string* err = nullptr);

  // 通过 HID 控制通道原样发送一段 JSON 文本，不添加 JSON-RPC 封装。
  // response_json 非空时短暂等待一条设备消息。
  bool send_json_command(const std::string& json, std::string* response_json,
                         std::string* err = nullptr);

  // 阻塞等待一条设备消息。
  bool read_control_message(int timeout_ms, std::string* json,
                            std::string* err = nullptr);

  Stats stats() const;
  void reset_stats();

  void set_trace(TraceLevel level, TraceSink sink = TraceSink());
  TraceLevel trace_level() const;

  void on_event(EventCallback cb);

  // 最近一次解析到的 HID 节点与媒体端点，不可用时为空字符串。
  std::string hidraw_path() const;         // control (IF0)
  std::string video_hidraw_path() const;   // video (IF1)
  std::string media_endpoint() const;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace usbdisplay
