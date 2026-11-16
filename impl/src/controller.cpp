// SPDX-License-Identifier: MIT
#include "usbdisplay/device.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <random>
#include <thread>
#include <unistd.h>
#include <vector>

#include "audio/audio_backend.h"
#include "audio/audio_open_retry.h"
#include "audio/audio_pump.h"
#include "audio/default_sink_guard.h"
#include "audio/pulse_card.h"
#include "media_plan.h"
#include "common/json.h"
#include "common/log.h"
#include "common/util.h"
#include "control/control_channel.h"
#include "control/hid_channel.h"
#include "control/hid_report_channel.h"
#include "control/jsonrpc.h"
#include "hid/hid_profile.h"
#include "hid/report_sink.h"
#include "transport/hidraw.h"
#include "transport/retry.h"
#include "transport/transport.h"
#include "transport/usb_reset.h"
#include "transport/usb_video.h"
#include "usbdisplay/protocol.h"
#include "usbdisplay/version.h"

namespace usbdisplay {
namespace {

constexpr int kHeartbeatIntervalMs = 1200;  // verified app sends every ~1.5 s
constexpr int kNotificationPollMs = 50;

// 心跳参数：接收器会回传相同的数字字符串。
std::string make_session_id() {
  const uint64_t value = (mono_ms() % 900000ULL) + 100000ULL;
  return std::to_string(value);
}

std::string local_hostname() {
  char buf[256] = {0};
  if (::gethostname(buf, sizeof(buf) - 1) != 0) return "linux";
  return buf;
}

bool has_annexb_start_code(const uint8_t* data, size_t len) {
  if (data == nullptr || len < 3) return false;
  if (data[0] == 0 && data[1] == 0 && data[2] == 1) return true;
  return len >= 4 && data[0] == 0 && data[1] == 0 && data[2] == 0 &&
         data[3] == 1;
}

size_t bytes_per_sample(AudioFormat format) {
  return format == AudioFormat::PCM_F32LE ? 4 : 2;
}

const char* codec_name(Codec codec) {
  return codec == Codec::HEVC ? "hevc" : "h264";
}

}  // namespace

struct Controller::Impl {
  mutable std::mutex state_mutex;
  mutable std::mutex send_mutex;
  mutable std::mutex packet_mutex;
  mutable std::mutex control_mutex;
  mutable std::mutex event_mutex;

  DeviceSelector selector;
  VideoConfig video;
  AudioConfig audio;
  std::string wire_format_name = "legacy-v1";
  proto::PacketizeOptions video_options;
  proto::PacketizeOptions audio_options;
  proto::Packetizer video_packetizer;
  proto::Packetizer audio_packetizer;

  std::unique_ptr<control::ControlChannel> control;
  std::unique_ptr<control::JsonRpcClient> rpc;
  std::unique_ptr<transport::MediaSink> video_sink;
  std::unique_ptr<transport::MediaSink> audio_sink;  // null when multiplexed
  // USB 模式使用的通道对象。
  std::unique_ptr<transport::UsbVideoSink> usb_video;
  std::unique_ptr<audio::AudioSink> audio_output;
  std::unique_ptr<audio::AudioPump> audio_pump;
  bool video_configured = false;
  std::string audio_backend;
  std::string audio_device;
  std::string audio_open_error;
  bool audio_attempted = false;
  std::unique_ptr<audio::PulseDiscovery> pulse_discovery;
  std::unique_ptr<audio::DefaultSinkGuard> sink_guard;
  std::string audio_startup_default;
  bool audio_startup_captured = false;
  std::unique_ptr<audio::PulseCardControl> card_control;
  std::unique_ptr<audio::UsbCardProfileGuard> card_guard;
  bool card_prompted = false;
  uint64_t audio_started_us = 0;
  std::string control_path;
  bool usb_hid_mode = false;

  std::atomic<bool> is_open{false};
  std::atomic<bool> streaming{false};
  EventCallback event_cb;
  TraceLevel trace = TraceLevel::Warn;

  std::thread heartbeat_thread;
  std::mutex heartbeat_mutex;
  std::condition_variable heartbeat_cv;
  bool heartbeat_stop = false;

  std::string session_id;
  uint32_t video_frame_id = 0;
  uint32_t audio_frame_id = 0;
  uint64_t opened_at_ms = 0;
  uint16_t video_port = 0;
  uint16_t audio_port = 0;
  std::string media_endpoint;

  std::atomic<uint64_t> video_frames{0};
  std::atomic<uint64_t> video_bytes{0};
  std::atomic<uint64_t> video_packets{0};
  std::atomic<uint64_t> video_keyframes{0};
  std::atomic<uint64_t> audio_frames{0};
  std::atomic<uint64_t> audio_bytes{0};
  std::atomic<uint64_t> audio_packets{0};
  std::atomic<uint64_t> control_tx{0};
  std::atomic<uint64_t> control_rx{0};
  std::atomic<uint64_t> send_errors{0};
  std::atomic<uint64_t> heartbeat_tx{0};
  std::atomic<uint64_t> heartbeat_failures{0};
  std::atomic<uint64_t> last_video_pts{0};
  std::atomic<uint64_t> last_audio_pts{0};

  void emit(EventType type, const std::string& message, int code = 0) {
    EventCallback cb;
    {
      std::lock_guard<std::mutex> lock(event_mutex);
      cb = event_cb;
    }
    if (!cb) return;
    Event event;
    event.type = type;
    event.message = message;
    event.code = code;
    cb(event);
  }

  proto::WireFormat wire_format(std::string* err) const {
    proto::WireFormat format = proto::WireFormat::LegacyEzV1;
    if (!proto::wire_format_from_name(wire_format_name, &format)) {
      if (err) *err = "unknown wire format: " + wire_format_name;
      format = proto::WireFormat::LegacyEzV1;
    }
    return format;
  }

  void configure_packetizers() {
    proto::WireFormat format = proto::WireFormat::LegacyEzV1;
    proto::wire_format_from_name(wire_format_name, &format);
    const bool tcp = selector.media_transport == MediaTransport::Tcp;
    video_options = proto::PacketizeOptions();
    video_options.format = format;
    video_options.max_payload = selector.max_payload == 0 ? 1400
                                                          : selector.max_payload;
    video_options.stream_id = 0;
    video_options.length_prefix = tcp && format == proto::WireFormat::LegacyEzV1;
    video_packetizer.set_options(video_options);

    audio_options = video_options;
    audio_options.stream_id = 1;
    audio_packetizer.set_options(audio_options);
  }

  json::Value stream_params() const {
    json::Value params = json::Value::object();
    params.set("session-id", json::Value(session_id));
    params.set("width", json::Value(static_cast<int>(video.width)));
    params.set("height", json::Value(static_cast<int>(video.height)));
    params.set("fps", json::Value(static_cast<int>(video.fps)));
    params.set("bitrate", json::Value(static_cast<double>(video.bitrate_bps)));
    params.set("codec", json::Value(codec_name(video.codec)));
    params.set("fullscreen", json::Value(video.fullscreen));
    json::Value audio_json = json::Value::object();
    audio_json.set("sample_rate",
                   json::Value(static_cast<int>(audio.sample_rate)));
    audio_json.set("channels", json::Value(static_cast<int>(audio.channels)));
    audio_json.set("format",
                   json::Value(audio.format == AudioFormat::PCM_F32LE
                                   ? "f32le"
                                   : "s16le"));
    audio_json.set("frame_ms", json::Value(static_cast<int>(audio.frame_ms)));
    params.set("audio", audio_json);
    return params;
  }

  json::Value stream_config_json() const {
    json::Value cfg = json::Value::object();
    cfg.set("type", json::Value("stream-config"));
    cfg.set("session-id", json::Value(session_id));
    json::Value video_json = json::Value::object();
    video_json.set("codec", json::Value(codec_name(video.codec)));
    video_json.set("width", json::Value(static_cast<int>(video.width)));
    video_json.set("height", json::Value(static_cast<int>(video.height)));
    video_json.set("fps", json::Value(static_cast<int>(video.fps)));
    video_json.set("bitrate",
                   json::Value(static_cast<double>(video.bitrate_bps)));
    cfg.set("video", video_json);
    cfg.set("audio", stream_params()["audio"]);
    return cfg;
  }

  bool wait_for_control_node(std::string* err) {
    if (!selector.control_hidraw_path.empty()) {
      const uint64_t deadline =
          mono_ms() + static_cast<uint64_t>(selector.auto_reset_wait_ms);
      while (::access(selector.control_hidraw_path.c_str(), R_OK | W_OK) != 0) {
        if (mono_ms() >= deadline) {
          if (err) {
            *err = "control hidraw node " + selector.control_hidraw_path +
                   " is not accessible; install the udev rule with "
                   "sudo tools/install-video-rules.sh";
          }
          return false;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
      }
      control_path = selector.control_hidraw_path;
      return true;
    }
    transport::HidrawInfo info;
    if (!transport::wait_for_hidraw_interface(
            selector.vid, selector.pid, 0, selector.auto_reset_wait_ms, &info,
            err)) {
      return false;
    }
    control_path = info.path;
    return true;
  }

  bool open_control_channel(std::string* err) {
    if (control && control->connected()) return true;
    if (control) control->close();
    control.reset();
    rpc.reset();
    control_path.clear();
    std::unique_ptr<hid::ReportSink> control_sink;
    if (!selector.control_report_dump_path.empty()) {
      control_sink = hid::make_recording_report_sink(
          selector.control_report_dump_path, selector.control_report_id,
          selector.control_report_size, true, 0);
    } else {
      if (!wait_for_control_node(err)) return false;
      control_sink = hid::make_hidraw_report_sink(
          control_path, selector.control_report_id, selector.control_report_size,
          true, 0);
    }
    auto channel = std::unique_ptr<control::HidReportControlChannel>(
        new control::HidReportControlChannel(std::move(control_sink)));
    if (!channel->open(err)) return false;
    control = std::move(channel);
    rpc.reset(new control::JsonRpcClient(*control));
    emit(EventType::DeviceFound, control->describe());
    log::info("control: " + control->describe());
    return true;
  }

  // 复位接收器并等待重新枚举，调用前必须关闭视频通道与控制通道。
  bool auto_reset_and_rediscover(std::string* err) {
    if (!usb_hid_mode || selector.mode == LinkMode::IpOnly ||
        !selector.report_dump_path.empty()) {
      if (err) *err = "auto-reset is only available for a real USB device";
      return false;
    }
    // 复位前先停止音频线程，检查默认输出，然后关闭音频输出设备。
    stop_audio(false);
    restore_usb_card();
    if (sink_guard) {
      std::string guard_err;
      if (!sink_guard->check_and_restore(&guard_err)) {
        log::warn("默认输出保护检查失败：" + guard_err);
      }
    }
    if (audio_output) {
      audio_output->close();
      audio_output.reset();
    }
    if (usb_video) usb_video->close();
    if (control) control->close();
    control.reset();
    rpc.reset();
    control_path.clear();
    if (!transport::usb_reset_device(selector.vid, selector.pid, err)) {
      return false;
    }
    if (!transport::wait_for_usb_device(selector.vid, selector.pid,
                                        selector.auto_reset_wait_ms, err)) {
      return false;
    }
    if (!selector.disable_control) {
      transport::HidrawInfo info;
      if (!transport::wait_for_hidraw_interface(
              selector.vid, selector.pid, 0, selector.auto_reset_wait_ms, &info,
              err)) {
        return false;
      }
    }
    return true;
  }

  bool setup_usb_hid(std::string* err) {
    usb_hid_mode = true;
    const bool allow_hardware = selector.mode != LinkMode::IpOnly;
    if (!selector.video_hidraw_path.empty()) {
      log::warn("video_hidraw_path is ignored: video now uses libusb EP2 OUT");
    }

    // 视频通道通过 libusb 向端点 2 输出方向发送 4096 字节 URB。
    // 离线演练改为写入转储文件，不访问硬件。
    transport::VideoUrbOptions urb;
    urb.vid = selector.vid;
    urb.pid = selector.pid;
    urb.interface_number = 1;
    urb.urb_size = selector.video_report_size;
    urb.report_id = selector.video_report_id;
    urb.transfer_mode = selector.usb_transfer;
    if (!selector.report_dump_path.empty()) {
      usb_video = transport::make_dump_video_sink(selector.report_dump_path,
                                                  urb);
    } else if (allow_hardware) {
      usb_video = transport::make_libusb_video_sink(urb);
    } else {
      usb_video = transport::make_null_video_sink(urb);
    }
    if (!usb_video) {
      if (err) *err = "failed to create USB video sink";
      return false;
    }
    media_endpoint = usb_video->describe();

    if (!selector.disable_control && selector.mode != LinkMode::IpOnly) {
      std::string control_err;
      if (!open_control_channel(&control_err)) {
        if (selector.mode == LinkMode::UsbOnly) {
          if (err) *err = control_err;
          return false;
        }
        log::warn("usb-hid: control channel open failed: " + control_err);
      }
    }
    log::info("usb-hid: video=" + media_endpoint +
              " control=" + (control ? control->describe() : "none"));
    return true;
  }

  bool release_usb_card() {
    if (card_guard && card_guard->disabled()) return true;
    if (!card_control) card_control = audio::make_libpulse_card_control();
    if (!card_guard) {
      card_guard.reset(new audio::UsbCardProfileGuard(card_control.get()));
    }
    std::string card_err;
    if (card_guard->disable(&card_err)) {
      if (card_guard->changed() && !card_prompted) {
        log::warn("PulseAudio 仍占用 USB 声卡，已临时关闭其 profile；"
                  "建议执行 sudo tools/install-audio-isolation.sh 并重新插拔");
        card_prompted = true;
      }
      return card_guard->disabled();
    }
    if (card_err.find("未找到") == std::string::npos) {
      log::warn("临时释放 USB 声卡失败：" + card_err);
    }
    return false;
  }

  void restore_usb_card() {
    if (!card_guard || !card_guard->disabled()) return;
    std::string card_err;
    if (!card_guard->restore(&card_err)) {
      log::warn("恢复 USB 声卡 profile 失败：" + card_err);
    }
  }

  bool open_audio(std::string* err) {
    if (!selector.enable_audio || selector.alsa_device.empty()) return true;
    audio_attempted = true;
    if (audio_output && audio_output->is_open()) return true;
    if (audio_output) {
      audio_output->close();
      audio_output.reset();
    }
    audio::AudioOptions options;
    options.backend = selector.audio_backend;
    options.pulse_sink = selector.pulse_sink;
    options.alsa_device = selector.alsa_device;
    options.sample_rate = audio.sample_rate;
    options.channels = audio.channels;
#ifdef USBDISPLAY_HAVE_PULSE
    options.pulse_available = true;
#else
    options.pulse_available = false;
#endif
    if (!pulse_discovery) {
      pulse_discovery = audio::make_libpulse_discovery();
    }
    if (!card_control) {
      card_control = audio::make_libpulse_card_control();
    }
    bool card_visible = false;
    if (options.pulse_available) {
      std::vector<audio::PulseCardInfo> cards;
      std::string card_err;
      if (card_control->list_cards(&cards, &card_err)) {
        card_visible = audio::has_usb_card(cards);
      } else {
        log::debug("查询 PulseAudio 声卡失败：" + card_err);
      }
    }
    std::string selected_sink = selector.pulse_sink;
    if (options.pulse_available && selector.audio_backend != "alsa") {
      std::vector<audio::PulseSinkInfo> sinks;
      std::string sink_err;
      if (pulse_discovery->list_sinks(&sinks, &sink_err)) {
        if (selected_sink.empty()) {
          selected_sink = audio::select_usb_sink(sinks);
        }
      } else if (selector.audio_backend == "pulse") {
        log::warn("PulseAudio sink 查询失败：" + sink_err);
      }
    }
    const std::string backend = audio::choose_audio_backend(
        selector.audio_backend, options.pulse_available, card_visible,
        selected_sink);
    options.backend = backend;
    options.pulse_sink = selected_sink;
    audio::AudioSinkCreator creator =
        [](const std::string& device, uint32_t rate, uint16_t channels,
           bool pulse, std::string* e) -> std::unique_ptr<audio::AudioSink> {
      if (pulse) {
        return audio::make_audio_sink(device, rate, channels, true, e);
      }
      std::unique_ptr<audio::AudioSink> sink;
      audio::OpenAttempt attempt = [&](std::string* attempt_err) {
        sink = audio::make_audio_sink(device, rate, channels, false,
                                      attempt_err);
        return sink != nullptr;
      };
      audio::OpenRetryPolicy policy;
      const audio::OpenRetryResult retry = audio::open_with_retry(
          policy, attempt, [](int interval_ms) {
            std::this_thread::sleep_for(std::chrono::milliseconds(interval_ms));
          });
      if (!retry.ok) {
        if (e) *e = retry.error;
        return nullptr;
      }
      return sink;
    };
    audio::AudioOutput output =
        audio::create_audio_output(options, pulse_discovery.get(), creator);
    if (!output.sink && backend == "alsa") {
      const std::string first_error = output.error;
      if (release_usb_card()) {
        output = audio::create_audio_output(options, pulse_discovery.get(),
                                            creator);
        if (!output.sink) {
          restore_usb_card();
          output.error =
              first_error + "；card profile off 后仍失败：" + output.error;
        }
      }
    }
    audio_backend = output.backend;
    audio_device = output.device;
    audio_open_error = output.error;
    if (!output.sink) {
      restore_usb_card();
      log::warn("音频输出不可用：" + output.error);
      if (selector.require_audio) {
        if (err) *err = output.error;
        return false;
      }
      return true;
    }
    audio_output = std::move(output.sink);
    audio_started_us = mono_us();
    log::info("音频输出：" + audio_backend + " " + audio_device);
    log::info("音频启动时间戳：" + std::to_string(audio_started_us) + " us");
    if (output.backend == "pulse") {
      if (!sink_guard) {
        sink_guard.reset(new audio::DefaultSinkGuard(
            pulse_discovery.get(), audio_device,
            selector.restore_default_sink));
        std::string guard_err;
        if (audio_startup_captured) {
          if (!sink_guard->capture_with_default(audio_startup_default,
                                                &guard_err)) {
            log::warn("默认输出保护未启用：" + guard_err);
          }
        } else {
          log::warn("默认输出未在开流前保存，保护未启用");
        }
      }
      if (sink_guard) {
        std::string guard_err;
        if (!sink_guard->check_and_restore(&guard_err)) {
          log::warn("默认输出保护检查失败：" + guard_err);
        }
      }
    }
    return true;
  }

  bool start_audio(std::string* err) {
    if (!audio_output || !audio_output->is_open()) return true;
    if (audio_pump) return true;
    audio::AudioPumpConfig config;
    if (sink_guard) {
      config.periodic_check = [this]() {
        std::string guard_err;
        if (!sink_guard->check_and_restore(&guard_err)) {
          log::warn("默认输出保护检查失败：" + guard_err);
        }
      };
    }
    audio_pump.reset(new audio::AudioPump(audio_output.get(), config));
    if (!audio_pump->start(err)) {
      audio_pump.reset();
      return false;
    }
    return true;
  }

  void stop_audio(bool drain) {
    if (audio_pump) {
      audio_pump->stop(drain);
      audio_pump.reset();
    }
  }

  bool run_handshake(std::string* err) {
    if (!control || !rpc) return true;
    struct Step {
      std::string method;
      json::Value params;
      bool notification;
      bool required;
    };
    std::vector<Step> steps;
    {
      // IP 网络传输路径的握手步骤：先分配主机角色，再请求视频流。
      json::Value role = json::Value::object();
      role.set("role", json::Value("host"));
      role.set("session-id", json::Value(session_id));
      steps.push_back({"common.assign_role", role, false, false});
      json::Value request = stream_params();
      request.set("split-count", json::Value(1));
      request.set("split-position", json::Value(1));
      request.set("role", json::Value("host"));
      steps.push_back({"common.request_stream", request, false,
                       selector.require_handshake});
    }
    bool required_failed = false;
    for (const auto& step : steps) {
      std::lock_guard<std::mutex> lock(control_mutex);
      std::string step_err;
      bool ok = false;
      if (step.notification) {
        ok = rpc->notify(step.method, step.params, &step_err);
      } else {
        json::Value result;
        ok = rpc->call(step.method, step.params, &result, &step_err);
      }
      if (ok) {
        control_tx.fetch_add(1);
        log::info("control: " + step.method + " ok");
      } else {
        log::warn("control: " + step.method + " failed: " + step_err);
        if (step.required) required_failed = true;
      }
    }
    if (required_failed) {
      if (err) *err = "control handshake failed";
      return false;
    }
    return true;
  }

  bool open_sinks(std::string* err) {
    if (!video_sink) {
      if (err) *err = "no media sink configured";
      return false;
    }
    if (!video_sink->connected() && !video_sink->open(err)) return false;
    if (audio_sink && !audio_sink->connected() && !audio_sink->open(err)) {
      return false;
    }
    return true;
  }

  void close_sinks() {
    std::lock_guard<std::mutex> lock(send_mutex);
    if (video_sink) video_sink->close();
    if (audio_sink) audio_sink->close();
  }

  void heartbeat_loop() {
    std::unique_lock<std::mutex> lock(heartbeat_mutex);
    while (!heartbeat_stop) {
      heartbeat_cv.wait_for(lock, std::chrono::milliseconds(kHeartbeatIntervalMs),
                            [this] { return heartbeat_stop; });
      if (heartbeat_stop) break;
      lock.unlock();
      do_heartbeat();
      lock.lock();
    }
  }

  bool send_heartbeat_now(std::string* err) {
    if (!control) {
      if (err) *err = "no control channel";
      return false;
    }
    // 心跳消息必须携带数字形式的会话编号，接收器会在回复中回传该编号。
    json::Value params = json::Value::object();
    params.set("session-id", json::Value(session_id));
    json::Value request = json::Value::object();
    request.set("jsonrpc", json::Value("2.0"));
    request.set("method", json::Value("heartbeat"));
    request.set("params", params);
    if (!control->send_json(request.dump(), err)) return false;
    heartbeat_tx.fetch_add(1);
    control_tx.fetch_add(1);
    return true;
  }

  void handle_notification(const control::RpcMessage& message) {
    if (message.has_id) {
      // 接收器发来带编号的请求，需要回复。投屏请求返回允许或拒绝，
      // 其他请求返回空结果。
      json::Value response = json::Value::object();
      response.set("jsonrpc", json::Value("2.0"));
      if (message.method == "projection-request-with-tx" ||
          message.method == "projection-request-cancel-with-tx") {
        json::Value result = json::Value::object();
        result.set("action", json::Value("allow"));
        response.set("result", result);
      } else {
        response.set("result", json::Value::object());
      }
      response.set("id", json::Value(message.id));
      std::string send_err;
      if (control->send_json(response.dump(), &send_err)) {
        control_tx.fetch_add(1);
        log::info("control: answered " + message.method + " id=" +
                  std::to_string(message.id));
      } else {
        log::warn("control: answer failed: " + send_err);
      }
    }
    Event event;
    event.type = EventType::ControlMessage;
    event.method = message.method;
    event.json = message.params.dump();
    log::info("control: notification " + message.method + " " +
              event.json.substr(0, 200));
    if (message.method == "notify-app-resend-stream-iframe-metadata-info") {
      Event kf;
      kf.type = EventType::KeyframeRequested;
      kf.message = "receiver asked to resend SPS/PPS+IDR";
      EventCallback kcb;
      {
        std::lock_guard<std::mutex> lock(event_mutex);
        kcb = event_cb;
      }
      if (kcb) kcb(kf);
    } else if (message.method == "notify-streaming-codec-need-change") {
      log::warn("control: receiver requested a codec change");
    }
    EventCallback cb;
    {
      std::lock_guard<std::mutex> lock(event_mutex);
      cb = event_cb;
    }
    if (cb) cb(event);
  }

  // 读取控制消息，直到超时或收到指定编号的响应。调用方必须持有控制锁。
  bool pump_control(int timeout_ms, int match_id, json::Value* result,
                    bool* matched) {
    if (matched) *matched = false;
    const uint64_t deadline = mono_ms() + static_cast<uint64_t>(timeout_ms);
    while (mono_ms() < deadline) {
      control::RpcMessage message;
      bool timed_out = false;
      std::string recv_err;
      const int remaining = static_cast<int>(deadline - mono_ms());
      if (remaining <= 0) break;
      if (!rpc->recv(&message, remaining, &timed_out, &recv_err)) {
        if (timed_out) break;
        log::debug("control recv: " + recv_err);
        return false;
      }
      if (message.is_response) {
        if (match_id >= 0 && message.has_id && message.id == match_id) {
          if (result) *result = message.result;
          if (matched) *matched = true;
          return true;
        }
        continue;
      }
      control_rx.fetch_add(1);
      handle_notification(message);
    }
    return true;
  }

  bool send_request_with_id(const std::string& method,
                            const json::Value& params, int id,
                            int timeout_ms, json::Value* result, bool* got,
                            std::string* err) {
    if (!control || !rpc) {
      if (err) *err = "no control channel";
      return false;
    }
    json::Value request = json::Value::object();
    request.set("jsonrpc", json::Value("2.0"));
    request.set("method", json::Value(method));
    if (!params.is_null()) request.set("params", params);
    request.set("id", json::Value(id));
    std::lock_guard<std::mutex> lock(control_mutex);
    if (!control->send_json(request.dump(), err)) return false;
    control_tx.fetch_add(1);
    return pump_control(timeout_ms, id, result, got);
  }

  bool send_notification_with_pump(const std::string& method,
                                   const json::Value& params, int pump_ms,
                                   std::string* err) {
    if (!control || !rpc) {
      if (err) *err = "no control channel";
      return false;
    }
    json::Value request = json::Value::object();
    request.set("jsonrpc", json::Value("2.0"));
    request.set("method", json::Value(method));
    if (!params.is_null()) request.set("params", params);
    std::lock_guard<std::mutex> lock(control_mutex);
    if (!control->send_json(request.dump(), err)) return false;
    control_tx.fetch_add(1);
    return pump_control(pump_ms, -1, nullptr, nullptr);
  }

  // 设备启动序列：先发送携带会话编号的心跳，再依次发送 app-launched、
  // 鼠标图标变更、两次投影停止通知、数据透传与最后一次 app-launched，
  // 最后发送 notify-projection-started 并等待 isAllowed 结果。
  bool run_verified_handshake(std::string* err) {
    if (!control || !rpc) {
      if (err) *err = "no control channel for handshake";
      return false;
    }
    std::string step_err;
    {
      std::lock_guard<std::mutex> lock(control_mutex);
      if (!send_heartbeat_now(&step_err)) {
        if (err) *err = "heartbeat: " + step_err;
        return false;
      }
    }
    {
      std::lock_guard<std::mutex> lock(control_mutex);
      pump_control(400, -1, nullptr, nullptr);
    }

    json::Value launched = json::Value::object();
    launched.set("platform", json::Value("windows"));
    launched.set("platform-machine-name", json::Value(local_hostname()));
    launched.set("product-type", json::Value("QuattroTx CT10"));
    launched.set("res-height", json::Value(static_cast<int>(video.height)));
    launched.set("res-width", json::Value(static_cast<int>(video.width)));

    json::Value result;
    bool got = false;
    if (!send_request_with_id("app-launched", launched, 1, 2000, &result, &got,
                              &step_err)) {
      if (err) *err = "app-launched id1: " + step_err;
      return false;
    }
    if (!got) {
      if (err) *err = "app-launched id1: no reply";
      return false;
    }

    json::Value mouse = json::Value::object();
    mouse.set("cursor_base64_data", json::Value(""));
    send_notification_with_pump("app_tx_mouse_icon_change", mouse, 150,
                                &step_err);
    send_notification_with_pump("notify-projection-stopped", json::Value(),
                                100, &step_err);
    json::Value passthrough = json::Value::object();
    passthrough.set("data", json::Value("AQ=="));
    passthrough.set("id", json::Value(103));
    passthrough.set("size", json::Value(1));
    send_notification_with_pump("common.data_passthrough", passthrough, 350,
                                &step_err);
    send_notification_with_pump("notify-projection-stopped", json::Value(),
                                100, &step_err);

    got = false;
    if (!send_request_with_id("app-launched", launched, 3, 2000, &result, &got,
                              &step_err)) {
      if (err) *err = "app-launched id3: " + step_err;
      return false;
    }
    if (!got) {
      if (err) *err = "app-launched id3: no reply";
      return false;
    }

    result = json::Value();
    got = false;
    if (!send_request_with_id("notify-projection-started", json::Value(), 4,
                              5000, &result, &got, &step_err)) {
      if (err) *err = "notify-projection-started: " + step_err;
      return false;
    }
    if (!got) {
      if (err) {
        *err = "no reply to notify-projection-started (isAllowed not received)";
      }
      return false;
    }
    const bool allowed =
        (result.is_string() && result.as_string() == "isAllowed") ||
        (result.is_bool() && result.as_bool(false));
    if (!allowed) {
      if (err) *err = "notify-projection-started result: " + result.dump();
      return false;
    }
    log::info("verified handshake complete: isAllowed");
    return true;
  }

  void do_heartbeat() {
    if (!control || !rpc || !control->connected()) return;
    std::lock_guard<std::mutex> lock(control_mutex);
    std::string err;
    if (!send_heartbeat_now(&err)) {
      heartbeat_failures.fetch_add(1);
      log::debug("control: heartbeat failed: " + err);
      return;
    }
    log::debug("control: heartbeat sent (session " + session_id + ")");
    pump_control(kNotificationPollMs, -1, nullptr, nullptr);
  }

  void start_heartbeat() {
    if (heartbeat_thread.joinable()) return;
    if (!control || !control->connected()) return;
    heartbeat_stop = false;
    heartbeat_thread = std::thread([this] { heartbeat_loop(); });
  }

  void stop_heartbeat() {
    {
      std::lock_guard<std::mutex> lock(heartbeat_mutex);
      heartbeat_stop = true;
    }
    heartbeat_cv.notify_all();
    if (heartbeat_thread.joinable()) heartbeat_thread.join();
  }
};

Controller::Controller() : impl_(new Impl()) {
  log::set_level(TraceLevel::Warn);
}

Controller::~Controller() { close(); }

DeviceSelector DeviceSelector::usb(uint16_t vid, uint16_t pid) {
  DeviceSelector selector;
  selector.vid = vid;
  selector.pid = pid;
  return selector;
}

DeviceSelector DeviceSelector::network(const std::string& host) {
  DeviceSelector selector;
  selector.mode = LinkMode::IpOnly;
  selector.host = host;
  return selector;
}

bool Controller::open(const DeviceSelector& selector, std::string* err) {
  Impl& d = *impl_;
  std::lock_guard<std::mutex> lock(d.state_mutex);
  if (d.is_open.load()) {
    d.stop_heartbeat();
    d.stop_audio(false);
    d.restore_usb_card();
    d.card_guard.reset();
    d.card_control.reset();
    if (d.sink_guard) {
      std::string guard_err;
      d.sink_guard->check_and_restore(&guard_err);
      d.sink_guard.reset();
    }
    if (d.audio_output) {
      d.audio_output->close();
      d.audio_output.reset();
    }
    d.pulse_discovery.reset();
    d.close_sinks();
    if (d.control) d.control->close();
    d.is_open = false;
  }
  d.selector = selector;
  d.session_id = make_session_id();
  d.opened_at_ms = mono_ms();
  d.video_frame_id = 0;
  d.audio_frame_id = 0;
  d.wire_format_name = selector.wire_format;
  d.control.reset();
  d.rpc.reset();
  d.video_sink.reset();
  d.audio_sink.reset();

  // 创建任何 PulseAudio 流之前保存默认 sink，避免 PulseAudio 在开流时自动切换后记录到错误值。
  d.audio_startup_default.clear();
  d.audio_startup_captured = false;
  if (selector.enable_audio && !selector.alsa_device.empty()) {
    if (!d.pulse_discovery) {
      d.pulse_discovery = audio::make_libpulse_discovery();
    }
    std::string sink_err;
    if (d.pulse_discovery->get_default_sink(&d.audio_startup_default, &sink_err)) {
      d.audio_startup_captured = !d.audio_startup_default.empty();
      if (d.audio_startup_captured) {
        log::info("开流前默认 sink：" + d.audio_startup_default);
      }
    } else {
      log::warn("保存默认 sink 失败：" + sink_err);
    }
  }

  proto::WireFormat format = proto::WireFormat::LegacyEzV1;
  if (!proto::wire_format_from_name(selector.wire_format, &format)) {
    if (err) *err = "unknown wire format '" + selector.wire_format + "'";
    return false;
  }
  if (format == proto::WireFormat::RtpAvc &&
      selector.media_transport == MediaTransport::Tcp) {
    if (err) *err = "wire format rtp requires UDP transport";
    return false;
  }

  const bool usb_hid = selector.media_transport == MediaTransport::UsbHid;
  if (usb_hid) {
    if (!d.setup_usb_hid(err)) return false;
  } else if (selector.mode != LinkMode::IpOnly) {
    control::HidChannelConfig cfg;
    cfg.path = selector.hidraw_path;
    cfg.vid = selector.vid;
    cfg.pid = selector.pid;
    cfg.report_id = selector.hid_report_id;
    cfg.report_size = selector.hid_report_size;
    cfg.use_feature_report = selector.hid_feature_report;
    cfg.chunked = selector.hid_chunked;
    cfg.timeout_ms = selector.control_timeout_ms;
    auto channel = std::unique_ptr<control::HidControlChannel>(
        new control::HidControlChannel(cfg));
    std::string control_err;
    if (channel->open(&control_err)) {
      d.control = std::move(channel);
      d.rpc.reset(new control::JsonRpcClient(*d.control));
      log::info("control: " + d.control->describe());
      d.emit(EventType::DeviceFound, d.control->describe());
    } else if (selector.mode == LinkMode::UsbOnly) {
      if (err) *err = control_err;
      return false;
    } else {
      log::warn("control: no HID control channel: " + control_err);
    }
  }

  // 媒体端点参数。
  const bool tcp = selector.media_transport == MediaTransport::Tcp;
  d.video_port = selector.video_port != 0
                     ? selector.video_port
                     : (tcp ? proto::kProvisionalMediaTcpPort
                            : proto::kProvisionalVideoUdpPort);
  d.audio_port = selector.audio_port != 0 ? selector.audio_port
                                          : proto::kProvisionalAudioUdpPort;
  if (!usb_hid) {
  d.configure_packetizers();

  switch (selector.media_transport) {
    case MediaTransport::Tcp: {
      d.video_sink = transport::make_tcp_sink(selector.host, d.video_port,
                                              selector.bind_address,
                                              selector.connect_timeout_ms);
      break;
    }
    case MediaTransport::Udp: {
      d.video_sink = transport::make_udp_sink(selector.host, d.video_port,
                                              selector.bind_address);
      if (selector.use_udp_audio) {
        d.audio_sink = transport::make_udp_sink(selector.host, d.audio_port,
                                                selector.bind_address);
      }
      break;
    }
    case MediaTransport::Loopback:
      d.video_sink = transport::make_loopback_sink(format);
      break;
    case MediaTransport::Null:
      d.video_sink = transport::make_null_sink();
      break;
    case MediaTransport::UsbHid:
      break;  // handled by setup_usb_hid
  }
  if (!d.video_sink) {
    if (err) *err = "failed to create media sink";
    return false;
  }
  d.media_endpoint = d.video_sink->describe();
  }  // !usb_hid
  d.is_open = true;
  d.emit(EventType::Opened,
         "session " + d.session_id + " media " + d.media_endpoint +
             " control " + (d.control ? d.control->describe() : "none"));
  log::info("usbdisplay " USBDISPLAY_VERSION_STRING ": opened session " +
            d.session_id + " media=" + d.media_endpoint +
            " control=" + (d.control ? d.control->describe() : "none"));
  return true;
}

void Controller::close() {
  Impl& d = *impl_;
  std::lock_guard<std::mutex> lock(d.state_mutex);
  if (!d.is_open.load() && !d.control && !d.video_sink && !d.usb_video &&
      !d.audio_pump && !d.audio_output && !d.sink_guard && !d.card_guard) {
    return;
  }
  const bool was_streaming = d.streaming.exchange(false);
  const bool reset_on_exit =
      d.selector.reset_on_exit && d.usb_hid_mode &&
      d.selector.mode != LinkMode::IpOnly &&
      d.selector.report_dump_path.empty();
  d.stop_heartbeat();
  d.stop_audio(false);
  d.restore_usb_card();
  d.close_sinks();
  if (d.control) d.control->close();
  if (was_streaming) {
    d.emit(EventType::StreamingStopped, "closed");
  }
  if (d.sink_guard) {
    std::string guard_err;
    d.sink_guard->check_and_restore(&guard_err);
    std::fprintf(stderr, "%s\n", d.sink_guard->exit_note().c_str());
    d.sink_guard.reset();
  } else if (d.audio_attempted) {
    if (d.audio_startup_captured) {
      std::fprintf(stderr, "默认输出已记录：%s，未修改\n",
                   d.audio_startup_default.c_str());
    } else {
      std::fprintf(stderr, "默认输出未修改\n");
    }
  }
  d.audio_attempted = false;
  d.card_guard.reset();
  d.card_control.reset();
  d.pulse_discovery.reset();
  if (d.audio_output) d.audio_output->close();
  d.audio_output.reset();
  if (d.usb_video) d.usb_video->close();
  d.usb_video.reset();
  d.control.reset();
  d.rpc.reset();
  d.video_sink.reset();
  d.audio_sink.reset();
  d.control_path.clear();
  d.usb_hid_mode = false;
  d.media_endpoint.clear();
  d.is_open = false;
  log::info("closed");
  if (reset_on_exit) {
    std::string reset_err;
    if (transport::usb_reset_device(d.selector.vid, d.selector.pid,
                                    &reset_err)) {
      log::info("auto-reset: device reset on exit");
    } else {
      log::warn("auto-reset on exit failed: " + reset_err);
    }
  }
}

bool Controller::is_open() const { return impl_->is_open.load(); }

bool Controller::configure(const VideoConfig& video, const AudioConfig& audio,
                           std::string* err) {
  Impl& d = *impl_;
  std::lock_guard<std::mutex> lock(d.state_mutex);
  if (!d.is_open.load()) {
    if (err) *err = "controller not open";
    return false;
  }
  if (!validate_media_config(video, audio, err)) {
    return false;
  }
  d.video = video;
  d.audio = audio;
  d.video_configured = video.width != 0 || video.height != 0 ||
                       video.fps != 0 || video.bitrate_bps != 0;
  if (!d.usb_hid_mode) d.configure_packetizers();
  d.emit(EventType::Configured,
         std::to_string(video.width) + "x" + std::to_string(video.height) +
             "@" + std::to_string(video.fps) + " " + codec_name(video.codec) +
             " " + std::to_string(video.bitrate_bps / 1000) + "kbps, audio " +
             std::to_string(audio.sample_rate) + "Hz/" +
             std::to_string(audio.channels) + "ch");
  log::info("configured: " + std::to_string(video.width) + "x" +
            std::to_string(video.height) + "@" + std::to_string(video.fps) +
            " " + codec_name(video.codec) + " audio " +
            std::to_string(audio.sample_rate) + "Hz");
  return true;
}

bool Controller::start(std::string* err) {
  Impl& d = *impl_;
  std::lock_guard<std::mutex> lock(d.state_mutex);
  if (!d.is_open.load()) {
    if (err) *err = "controller not open";
    return false;
  }
  if (d.streaming.load()) return true;
  if (d.usb_hid_mode) {
    const bool want_video = !d.selector.disable_video && d.video_configured;
    if (!d.usb_video && want_video) {
      if (err) *err = "no USB video sink";
      return false;
    }
    // 设备固件只接受重新枚举后的会话，失败时复位设备并重新枚举后重试打开与握手。
    transport::ResetRetryPolicy policy;
    policy.auto_reset = d.selector.auto_reset;
    policy.max_reset_attempts = d.selector.auto_reset_retries;
    policy.reset_wait_ms = d.selector.auto_reset_wait_ms;
    transport::ResetRetryCallbacks callbacks;
    callbacks.attempt = [&d, want_video](std::string* attempt_err) -> bool {
      if (want_video && !d.usb_video->connected() &&
          !d.usb_video->open(attempt_err)) {
        return false;
      }
      if (!d.selector.disable_control && d.selector.mode != LinkMode::IpOnly &&
          !d.control) {
        if (!d.open_control_channel(attempt_err)) return false;
      }
      if (d.selector.send_handshake) {
        if (!d.run_verified_handshake(attempt_err)) return false;
      }
      return true;
    };
    callbacks.reset = [&d](std::string* reset_err) -> bool {
      return d.auto_reset_and_rediscover(reset_err);
    };
    callbacks.log = [](int reset_attempt, const std::string& last_error) {
      log::warn("auto-reset: attempt " + std::to_string(reset_attempt) +
                " (previous attempt failed: " + last_error + ")");
    };
    callbacks.is_fatal = [](const std::string& error) -> bool {
      return error.find("permission") != std::string::npos ||
             error.find("udev") != std::string::npos ||
             error.find("LIBUSB_ERROR_ACCESS") != std::string::npos;
    };
    const transport::ResetRetryResult retry =
        transport::run_with_auto_reset(policy, callbacks);
    if (!retry.ok) {
      d.stop_audio(false);
      d.restore_usb_card();
      if (d.usb_video) d.usb_video->close();
      if (d.control) d.control->close();
      d.control.reset();
      d.rpc.reset();
      d.emit(EventType::Error, retry.error, 2);
      if (err) *err = retry.error;
      return false;
    }
    if (retry.resets > 0) {
      log::info("auto-reset: session recovered after " +
                std::to_string(retry.resets) + " reset(s)");
    }
    d.media_endpoint = want_video ? d.usb_video->describe() : "control-only";
    const bool audio_ok = d.open_audio(err);
    if (!audio_ok && d.selector.require_audio) return false;
    bool audio_started = false;
    if (audio_ok) {
      audio_started = d.start_audio(err);
      if (!audio_started) {
        d.stop_audio(false);
        d.restore_usb_card();
        if (d.audio_output) {
          d.audio_output->close();
          d.audio_output.reset();
        }
        if (d.selector.require_audio) {
          d.emit(EventType::Error, err ? *err : "音频播放线程启动失败", 3);
          return false;
        }
        log::warn("音频播放线程启动失败，继续视频：" +
                  (err ? *err : std::string("未知错误")));
      }
    }
    if (!audio_ok) {
      log::warn("音频不可用，继续视频：" +
                (err ? *err : std::string("未知错误")));
    }
    if (d.sink_guard) {
      std::string guard_err;
      if (!d.sink_guard->check_and_restore(&guard_err)) {
        log::warn("默认输出保护检查失败：" + guard_err);
      }
    }
    if (!d.selector.send_handshake &&
        d.selector.mode != LinkMode::IpOnly) {
      log::warn("handshake disabled: the receiver may ignore video "
                "(use --handshake; auto-reset cannot detect a stale session)");
    }
    d.streaming = true;
    d.start_heartbeat();
    d.emit(EventType::StreamingStarted, d.media_endpoint);
    log::info("USB verified-protocol streaming started -> " + d.media_endpoint);
    return true;
  }
  if (!d.open_sinks(err)) {
    d.emit(EventType::Error, err ? *err : "media open failed", 1);
    return false;
  }
  std::string handshake_err;
  if (!d.run_handshake(&handshake_err)) {
    if (d.selector.require_handshake) {
      d.emit(EventType::Error, handshake_err, 2);
      if (err) *err = handshake_err;
      return false;
    }
    log::warn("handshake: " + handshake_err + " (continuing)");
  }
  if (d.selector.send_stream_config) {
    std::vector<std::vector<uint8_t>> packets;
    {
      std::lock_guard<std::mutex> plock(d.packet_mutex);
      d.video_packetizer.packetize_config(d.stream_config_json().dump(), 0,
                                          packets);
    }
    std::lock_guard<std::mutex> slock(d.send_mutex);
    for (const auto& packet : packets) {
      std::string send_err;
      if (!d.video_sink->send(packet.data(), packet.size(), &send_err)) {
        log::warn("stream config packet failed: " + send_err);
        break;
      }
    }
  }
  d.streaming = true;
  d.start_heartbeat();
  d.emit(EventType::StreamingStarted, d.media_endpoint);
  log::info("streaming started -> " + d.media_endpoint);
  return true;
}

bool Controller::stop_audio() {
  Impl& d = *impl_;
  std::lock_guard<std::mutex> lock(d.state_mutex);
  d.stop_audio(true);
  d.restore_usb_card();
  log::info("audio stopped, video path continues");
  return true;
}

bool Controller::start_audio(std::string* err) {
  Impl& d = *impl_;
  std::lock_guard<std::mutex> lock(d.state_mutex);
  if (!d.streaming.load()) {
    if (err) *err = "not streaming (call start())";
    return false;
  }
  return d.start_audio(err);
}

bool Controller::stop() {
  Impl& d = *impl_;
  std::lock_guard<std::mutex> lock(d.state_mutex);
  if (!d.streaming.exchange(false)) {
    d.stop_audio(false);
    return true;
  }
  d.stop_heartbeat();
  d.stop_audio(true);
  d.close_sinks();
  d.emit(EventType::StreamingStopped, "stopped");
  log::info("streaming stopped");
  return true;
}

bool Controller::push_video(const uint8_t* annexb, size_t len, uint64_t pts_us,
                            bool keyframe, std::string* err) {
  Impl& d = *impl_;
  if (!d.streaming.load()) {
    if (err) *err = "not streaming (call start())";
    return false;
  }
  if (annexb == nullptr || len == 0) {
    if (err) *err = "empty video frame";
    return false;
  }
  if (d.usb_hid_mode) {
    if (!d.usb_video) {
      if (err) *err = "no USB video sink";
      return false;
    }
    // 访问单元必须保留 Annex-B 起始码，缺失起始码时拒绝发送。
    if (!has_annexb_start_code(annexb, len)) {
      if (err) {
        *err = "video access unit does not start with an Annex-B start code "
               "(00 00 01 / 00 00 00 01); refusing to drop start codes";
      }
      d.emit(EventType::Error, err ? *err : "missing Annex-B start code", 3);
      return false;
    }
    std::lock_guard<std::mutex> lock(d.send_mutex);
    const uint64_t before_packets = d.usb_video->packets_sent();
    if (!d.usb_video->send_frame(annexb, len, d.video.width, d.video.height,
                                 err)) {
      d.send_errors.fetch_add(1);
      return false;
    }
    d.video_packets.fetch_add(d.usb_video->packets_sent() - before_packets);
    d.video_bytes.fetch_add(len);
    d.video_frames.fetch_add(1);
    if (keyframe) d.video_keyframes.fetch_add(1);
    d.last_video_pts.store(pts_us);
    if (log::enabled(TraceLevel::Wire)) {
      log::wire("video URB msg len=" + std::to_string(len) + " " +
                hexdump(annexb, std::min<size_t>(len, 48), 48));
    }
    return true;
  }
  std::vector<std::vector<uint8_t>> packets;
  {
    std::lock_guard<std::mutex> lock(d.packet_mutex);
    d.video_packetizer.packetize(annexb, len, proto::MediaKind::Video, pts_us,
                                 keyframe, d.video_frame_id++, packets);
  }
  {
    std::lock_guard<std::mutex> lock(d.send_mutex);
    for (const auto& packet : packets) {
      if (!d.video_sink->send(packet.data(), packet.size(), err)) {
        d.send_errors.fetch_add(1);
        return false;
      }
      d.video_bytes.fetch_add(packet.size());
      d.video_packets.fetch_add(1);
      if (log::enabled(TraceLevel::Wire)) {
        log::wire("video pkt " + std::to_string(packet.size()) + "B " +
                  hexdump(packet.data(), packet.size(), 48));
      }
    }
  }
  d.video_frames.fetch_add(1);
  if (keyframe) d.video_keyframes.fetch_add(1);
  d.last_video_pts.store(pts_us);
  return true;
}

bool Controller::push_audio(const void* pcm, size_t bytes, uint64_t pts_us,
                            std::string* err) {
  Impl& d = *impl_;
  if (!d.streaming.load()) {
    if (err) *err = "not streaming (call start())";
    return false;
  }
  if (pcm == nullptr || bytes == 0) {
    if (err) *err = "empty audio buffer";
    return false;
  }
  if (d.usb_hid_mode) {
    if (!d.selector.enable_audio || d.selector.alsa_device.empty()) {
      log::debug("音频已关闭，丢弃 PCM 数据");
      return true;
    }
    if (!d.audio_pump) {
      if (err) {
        *err = d.audio_open_error.empty() ? "音频播放线程未启动"
                                          : ("音频输出未就绪：" + d.audio_open_error);
      }
      return false;
    }
    // 仅入队后立即返回，ALSA 写入由播放线程完成。
    if (!d.audio_pump->push(pcm, bytes, pts_us, err)) {
      log::warn("音频入队失败：" + (err ? *err : std::string("未知错误")));
      return false;
    }
    d.audio_frames.fetch_add(1);
    d.audio_bytes.fetch_add(bytes);
    d.audio_packets.fetch_add(1);
    d.last_audio_pts.store(pts_us);
    return true;
  }
  const size_t bps = bytes_per_sample(d.audio.format);
  const size_t frame_bytes =
      static_cast<size_t>(d.audio.sample_rate) * d.audio.frame_ms / 1000u *
      d.audio.channels * bps;
  if (frame_bytes == 0) {
    if (err) *err = "invalid audio frame size";
    return false;
  }
  const uint64_t frame_duration_us =
      static_cast<uint64_t>(d.audio.frame_ms) * 1000ULL;
  const auto* base = static_cast<const uint8_t*>(pcm);
  size_t offset = 0;
  uint64_t chunk_index = 0;
  std::lock_guard<std::mutex> send_lock(d.send_mutex);
  transport::MediaSink* sink =
      d.audio_sink ? d.audio_sink.get() : d.video_sink.get();
  while (offset < bytes) {
    const size_t n = std::min(frame_bytes, bytes - offset);
    std::vector<std::vector<uint8_t>> packets;
    {
      std::lock_guard<std::mutex> lock(d.packet_mutex);
      d.audio_packetizer.packetize(base + offset, n, proto::MediaKind::Audio,
                                   pts_us + chunk_index * frame_duration_us,
                                   false, d.audio_frame_id++, packets);
    }
    for (const auto& packet : packets) {
      if (!sink->send(packet.data(), packet.size(), err)) {
        d.send_errors.fetch_add(1);
        return false;
      }
      d.audio_bytes.fetch_add(packet.size());
      d.audio_packets.fetch_add(1);
      if (log::enabled(TraceLevel::Wire)) {
        log::wire("audio pkt " + std::to_string(packet.size()) + "B " +
                  hexdump(packet.data(), packet.size(), 48));
      }
    }
    offset += n;
    ++chunk_index;
    d.audio_frames.fetch_add(1);
  }
  d.last_audio_pts.store(pts_us);
  return true;
}

bool Controller::request_keyframe(std::string* err) {
  Impl& d = *impl_;
  if (!d.is_open.load()) {
    if (err) *err = "controller not open";
    return false;
  }
  // 请求关键帧时通知调用方，由调用方强制编码器输出 IDR。
  d.emit(EventType::KeyframeRequested, "caller should emit IDR");
  log::info("keyframe requested");
  return true;
}

bool Controller::send_control(const std::string& method,
                              const std::string& params_json,
                              std::string* response_json, std::string* err) {
  Impl& d = *impl_;
  if (!d.is_open.load() || !d.control || !d.rpc) {
    if (err) *err = "no control channel";
    return false;
  }
  json::Value params;
  if (!params_json.empty()) {
    std::string perr;
    if (!json::Value::parse(params_json, &params, &perr)) {
      if (err) *err = "invalid params JSON: " + perr;
      return false;
    }
  }
  std::lock_guard<std::mutex> lock(d.control_mutex);
  json::Value result;
  if (!d.rpc->call(method, params, &result, err)) return false;
  d.control_tx.fetch_add(1);
  d.control_rx.fetch_add(1);
  if (response_json) *response_json = result.dump();
  return true;
}

bool Controller::send_json_command(const std::string& json,
                                   std::string* response_json,
                                   std::string* err) {
  Impl& d = *impl_;
  if (!d.control) {
    if (err) *err = "no control channel";
    return false;
  }
  std::lock_guard<std::mutex> lock(d.control_mutex);
  if (!d.control->send_json(json, err)) return false;
  d.control_tx.fetch_add(1);
  if (response_json) {
    std::string response;
    bool timed_out = false;
    std::string recv_err;
    if (d.control->recv_json(&response, 500, &timed_out, &recv_err)) {
      d.control_rx.fetch_add(1);
      *response_json = response;
    }
  }
  return true;
}

bool Controller::read_control_message(int timeout_ms, std::string* json,
                                      std::string* err) {
  Impl& d = *impl_;
  if (!d.control) {
    if (err) *err = "no control channel";
    return false;
  }
  std::lock_guard<std::mutex> lock(d.control_mutex);
  bool timed_out = false;
  if (!d.control->recv_json(json, timeout_ms, &timed_out, err)) {
    if (timed_out && err) *err = "timeout";
    return false;
  }
  d.control_rx.fetch_add(1);
  return true;
}

std::string Controller::video_hidraw_path() const { return {}; }  // deprecated: video uses libusb EP2

Stats Controller::stats() const {
  const Impl& d = *impl_;
  Stats s;
  s.video_frames = d.video_frames.load();
  s.video_bytes = d.video_bytes.load();
  s.video_packets = d.video_packets.load();
  s.video_keyframes = d.video_keyframes.load();
  s.audio_frames = d.audio_frames.load();
  s.audio_bytes = d.audio_bytes.load();
  s.audio_packets = d.audio_packets.load();
  s.control_tx = d.control_tx.load();
  s.control_rx = d.control_rx.load();
  s.send_errors = d.send_errors.load();
  s.dropped_packets = 0;
  s.heartbeat_tx = d.heartbeat_tx.load();
  s.heartbeat_failures = d.heartbeat_failures.load();
  s.last_video_pts_us = d.last_video_pts.load();
  s.last_audio_pts_us = d.last_audio_pts.load();
  s.uptime_ms =
      d.opened_at_ms == 0 ? 0 : mono_ms() - d.opened_at_ms;
  return s;
}

void Controller::reset_stats() {
  Impl& d = *impl_;
  d.video_frames = 0;
  d.video_bytes = 0;
  d.video_packets = 0;
  d.video_keyframes = 0;
  d.audio_frames = 0;
  d.audio_bytes = 0;
  d.audio_packets = 0;
  d.control_tx = 0;
  d.control_rx = 0;
  d.send_errors = 0;
  d.heartbeat_tx = 0;
  d.heartbeat_failures = 0;
}

void Controller::set_trace(TraceLevel level, TraceSink sink) {
  impl_->trace = level;
  log::set_level(level);
  log::set_sink(std::move(sink));
}

TraceLevel Controller::trace_level() const { return impl_->trace; }

void Controller::on_event(EventCallback cb) {
  std::lock_guard<std::mutex> lock(impl_->event_mutex);
  impl_->event_cb = std::move(cb);
}

std::string Controller::hidraw_path() const {
  return impl_->control ? impl_->control->describe() : std::string();
}

std::string Controller::media_endpoint() const {
  return impl_->media_endpoint;
}

}  // namespace usbdisplay
