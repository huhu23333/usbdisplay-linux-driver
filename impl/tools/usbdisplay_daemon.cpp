// SPDX-License-Identifier: MIT
// 消费 daemon：只读共享内存帧，编码并通过 Controller 发送到 USB Display。
// 不直接访问 evdi；grabber 与 evdi 生命周期由 usbdisplay-evdi.service 管理。
#include <fcntl.h>
#include <libusb-1.0/libusb.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <cerrno>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <thread>

#include "common/log.h"
#include "common/util.h"
#include "evdi/send_cadence.h"
#include "evdi/shared_frame.h"
#include "evdi/x264_encoder.h"
#include "usbdisplay/device.h"
#include "usbdisplay/protocol.h"

namespace {

volatile std::sig_atomic_t g_stop = 0;
void on_signal(int) { g_stop = 1; }

constexpr uint64_t kHeartbeatTimeoutMs = 3000;

void print_usage() {
  std::printf(
      "usage: usbdisplay_daemon [options]\n"
      "  --shm-path PATH  共享帧路径（默认 /dev/shm/usbdisplay-frames）\n"
      "  --socket PATH    租约 socket（默认 /run/usbdisplay/frames.sock）\n"
      "  --control-socket PATH  控制 socket（默认 /run/usbdisplay/control.sock）\n"
      "  --trace LEVEL    off|error|warn|info|debug|wire\n"
      "  --help           显示帮助\n");
}

bool parse_trace(const std::string& text, usbdisplay::TraceLevel* out) {
  static const char* names[] = {"off", "error", "warn", "info", "debug", "wire"};
  for (int i = 0; i < 6; ++i) {
    if (text == names[i]) {
      *out = static_cast<usbdisplay::TraceLevel>(i);
      return true;
    }
  }
  return false;
}

void sleep_interruptible(int milliseconds) {
  for (int elapsed = 0; elapsed < milliseconds && !g_stop;) {
    const int chunk = std::min(50, milliseconds - elapsed);
    std::this_thread::sleep_for(std::chrono::milliseconds(chunk));
    elapsed += chunk;
  }
}

bool send_control_command(const std::string& path, const std::string& command,
                          std::string* reply) {
  const int fd = ::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
  if (fd < 0) return false;
  sockaddr_un address{};
  address.sun_family = AF_UNIX;
  if (path.size() >= sizeof(address.sun_path)) { ::close(fd); return false; }
  std::memcpy(address.sun_path, path.c_str(), path.size() + 1);
  int result = ::connect(fd, reinterpret_cast<sockaddr*>(&address), sizeof(address));
  if (result != 0 && errno == EINPROGRESS) {
    pollfd pfd{fd, POLLOUT, 0};
    if (::poll(&pfd, 1, 300) <= 0) { ::close(fd); return false; }
    int socket_error = 0;
    socklen_t length = sizeof(socket_error);
    if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &socket_error, &length) != 0 ||
        socket_error != 0) { ::close(fd); return false; }
  } else if (result != 0) {
    ::close(fd);
    return false;
  }
  const std::string line = command + "\n";
  if (::send(fd, line.data(), line.size(), MSG_NOSIGNAL) !=
      static_cast<ssize_t>(line.size())) { ::close(fd); return false; }
  reply->clear();
  char buffer[256];
  for (;;) {
    pollfd pfd{fd, POLLIN, 0};
    if (::poll(&pfd, 1, 300) <= 0) { ::close(fd); return false; }
    const ssize_t got = ::recv(fd, buffer, sizeof(buffer), 0);
    if (got <= 0) { ::close(fd); return false; }
    reply->append(buffer, static_cast<size_t>(got));
    if (reply->find('\n') != std::string::npos) break;
    if (reply->size() > 4096) { ::close(fd); return false; }
  }
  ::close(fd);
  return true;
}

bool usb_present(libusb_context* context) {
  libusb_device** devices = nullptr;
  const ssize_t count = libusb_get_device_list(context, &devices);
  if (count < 0) return false;
  bool found = false;
  for (ssize_t i = 0; i < count && !found; ++i) {
    libusb_device_descriptor descriptor;
    if (libusb_get_device_descriptor(devices[i], &descriptor) < 0) continue;
    if (descriptor.idVendor == usbdisplay::kDefaultVid &&
        descriptor.idProduct == usbdisplay::kDefaultPid) {
      found = true;
    }
  }
  libusb_free_device_list(devices, 1);
  return found;
}

struct Session {
  std::unique_ptr<usbdisplay::Controller> controller;
  std::unique_ptr<usbdisplay::evdi::X264Encoder> encoder;
  uint64_t last_seq = 0;
  uint64_t sent_frames = 0;
  bool active = false;
};

bool start_session(Session* session, std::string* err) {
  using namespace usbdisplay;
  session->last_seq = 0;
  session->sent_frames = 0;
  session->controller.reset(new Controller());
  session->controller->set_trace(TraceLevel::Info);
  DeviceSelector selector = DeviceSelector::usb();
  selector.media_transport = MediaTransport::UsbHid;
  selector.enable_audio = false;
  selector.require_audio = false;
  selector.disable_video = false;
  selector.auto_reset = true;
  selector.send_handshake = true;
  selector.require_handshake = true;
  if (!session->controller->open(selector, err)) return false;

  VideoConfig video;
  video.width = 1920;
  video.height = 1080;
  video.fps = 60;
  video.bitrate_bps = 4'000'000;
  AudioConfig audio;
  audio.sample_rate = 0;
  audio.channels = 0;
  audio.frame_ms = 0;
  if (!session->controller->configure(video, audio, err)) {
    session->controller->close();
    return false;
  }
  if (!session->controller->start(err)) {
    session->controller->close();
    return false;
  }
  session->encoder.reset(new evdi::X264Encoder());
  if (!session->encoder->open(1920, 1080, 60, 4'000'000, err)) {
    session->controller->close();
    session->encoder.reset();
    return false;
  }
  session->last_seq = 0;
  session->active = true;
  return true;
}

void stop_session(Session* session) {
  if (session->encoder) session->encoder->close();
  if (session->controller) {
    session->controller->stop();
    session->controller->close();
  }
  session->encoder.reset();
  session->controller.reset();
  session->active = false;
  session->last_seq = 0;
}

bool reset_encoder(Session* session, std::string* err) {
  if (!session->encoder) return false;
  session->encoder->close();
  if (!session->encoder->open(1920, 1080, 60, 4'000'000, err)) {
    return false;
  }
  session->last_seq = 0;
  return true;
}

}  // namespace

int main(int argc, char** argv) {
  using namespace usbdisplay;
  std::string shm_path = evdi::kDefaultSharedFramePath;
  std::string socket_path = evdi::kDefaultLeaseSocketPath;
  std::string control_socket_path = "/run/usbdisplay/control.sock";
  TraceLevel trace = TraceLevel::Warn;
  for (int i = 1; i < argc; ++i) {
    const std::string arg = argv[i];
    auto next = [&](std::string* out) -> bool {
      if (i + 1 >= argc) return false;
      *out = argv[++i];
      return true;
    };
    if (arg == "--shm-path") {
      if (!next(&shm_path)) return 2;
    } else if (arg == "--socket") {
      if (!next(&socket_path)) return 2;
    } else if (arg == "--control-socket") {
      if (!next(&control_socket_path)) return 2;
    } else if (arg == "--trace") {
      std::string value;
      if (!next(&value) || !parse_trace(value, &trace)) return 2;
    } else if (arg == "--help" || arg == "-h") {
      print_usage();
      return 0;
    } else {
      std::fprintf(stderr, "未知参数：%s\n", arg.c_str());
      print_usage();
      return 2;
    }
  }
  log::set_level(trace);
  std::signal(SIGINT, on_signal);
  std::signal(SIGTERM, on_signal);

  libusb_context* usb_context = nullptr;
  if (libusb_init(&usb_context) < 0) {
    log::warn("libusb 初始化失败");
    usb_context = nullptr;
  }

  evdi::SharedFrameReader reader;
  Session session;
  uint64_t last_generation = 0;
  uint64_t next_attempt_ms = 0;
  int failures = 0;
  std::string last_wait_error;
  bool last_present = false;
  uint64_t last_usb_check_ms = 0;
  bool reader_open = false;
  uint64_t last_reader_retry_ms = 0;
  uint64_t last_reader_log_ms = 0;
  bool control_connected = false;
  uint64_t next_control_attempt_ms = 0;
  std::string last_control_error;
  evdi::SendCadence cadence(60);

  while (!g_stop) {
    if (!reader_open) {
      const uint64_t now = mono_ms();
      if (now - last_reader_retry_ms < 500) {
        sleep_interruptible(50);
        continue;
      }
      last_reader_retry_ms = now;
      std::string err;
      if (!reader.open(shm_path, socket_path, &err)) {
        if (err != last_wait_error) {
          log::warn("等待共享帧：" + err);
          last_wait_error = err;
        }
        sleep_interruptible(500);
        continue;
      }
      last_wait_error.clear();
      last_generation = reader.generation();
      reader_open = true;
      const uint64_t now_open = mono_ms();
      if (last_reader_log_ms == 0 || now_open - last_reader_log_ms >= 5000) {
        log::info("已打开共享帧：" + shm_path);
        last_reader_log_ms = now_open;
      }
    }

    if (reader.file_replaced(shm_path) || !reader.socket_connected() ||
        reader.heartbeat_age_ms() > kHeartbeatTimeoutMs) {
      log::warn("共享帧文件已替换或 writer 心跳超时，重新等待");
      reader.close();
      reader_open = false;
      stop_session(&session);
      sleep_interruptible(200);
      continue;
    }

    const uint64_t now = mono_ms();
    bool present = last_present;
    if (usb_context != nullptr && now - last_usb_check_ms >= 250) {
      present = usb_present(usb_context);
      last_usb_check_ms = now;
      last_present = present;
    }
    if (!present) {
      if (session.active) {
        log::info("USB 已拔出，停止 Controller");
        stop_session(&session);
      }
      if (reader_open) {
        reader.close();
        reader_open = false;
      }
      if (control_connected) {
        std::string reply;
        if (send_control_command(control_socket_path, "DISCONNECT", &reply)) {
          log::info("已通知 evdi 服务 DISCONNECT");
        } else {
          log::warn("evdi 控制 DISCONNECT 失败，稍后/退出时重试");
        }
        control_connected = false;
      }
      sleep_interruptible(100);
      continue;
    }

    if (!control_connected) {
      const uint64_t now_control = mono_ms();
      if (now_control < next_control_attempt_ms) {
        sleep_interruptible(50);
        continue;
      }
      std::string reply;
      if (!send_control_command(control_socket_path, "CONNECT", &reply) ||
          reply.rfind("OK", 0) != 0) {
        if (last_control_error != reply) {
          log::warn("evdi 控制 CONNECT 失败：" + reply + "，稍后重试");
          last_control_error = reply;
        }
        next_control_attempt_ms = now_control + 1000;
        sleep_interruptible(100);
        continue;
      }
      last_control_error.clear();
      control_connected = true;
      log::info("已通知 evdi 服务 CONNECT");
    }

    if (!session.active) {
      if (now < next_attempt_ms) {
        sleep_interruptible(50);
        continue;
      }
      std::string err;
      if (!start_session(&session, &err)) {
        log::warn("Controller 启动失败：" + err + "，稍后重试");
        ++failures;
        const int delay = std::min(5000, 2000 + (failures - 1) * 500);
        next_attempt_ms = mono_ms() + static_cast<uint64_t>(delay);
        sleep_interruptible(100);
        continue;
      }
      failures = 0;
      last_generation = reader.generation();
      cadence.reset(mono_ms());
      log::info("Controller 已启动，开始消费共享帧");
    }

    const uint64_t cadence_now = mono_ms();
    if (!cadence.due(cadence_now)) {
      sleep_interruptible(5);
      continue;
    }
    cadence.advance(cadence_now);

    evdi::SharedFrameView frame;
    std::string err;
    const evdi::FrameReadResult result = reader.read(&frame, &err);
    if (result == evdi::FrameReadResult::NoFrame) {
      sleep_interruptible(5);
      continue;
    }
    if (result == evdi::FrameReadResult::Error) {
      log::warn("读取共享帧失败：" + err);
      reader.close();
      reader_open = false;
      stop_session(&session);
      // 虚拟显示可能被其他程序 DISCONNECT；USB 在线时主动恢复 CONNECT。
      if (control_connected) {
        std::string reply;
        if (!send_control_command(control_socket_path, "CONNECT", &reply) ||
            reply.rfind("OK", 0) != 0) {
          control_connected = false;
        }
      }
      sleep_interruptible(500);
      continue;
    }
    if (frame.generation != last_generation) {
      log::info("共享帧 generation 变化，重置编码器");
      last_generation = frame.generation;
      if (!reset_encoder(&session, &err)) {
        log::warn("重置编码器失败：" + err);
        reader.close();
        reader_open = false;
        stop_session(&session);
        sleep_interruptible(500);
        continue;
      }
    }
    if (frame.seq == session.last_seq) continue;

    evdi::EncodedAccessUnit unit;
    if (!session.encoder->encode(frame.data.data(), static_cast<int>(frame.stride),
                                 0, &unit, &err)) {
      log::warn("编码失败：" + err);
      reader.close();
      reader_open = false;
      stop_session(&session);
      sleep_interruptible(500);
      continue;
    }
    if (unit.data.empty()) continue;
    const uint64_t pts_us = cadence.pts_us(session.sent_frames);
    if (!session.controller->push_video(unit.data.data(), unit.data.size(),
                                        pts_us, unit.keyframe, &err)) {
      log::warn("发送失败：" + err);
      reader.close();
      reader_open = false;
      stop_session(&session);
      sleep_interruptible(500);
      continue;
    }
    session.last_seq = frame.seq;
    ++session.sent_frames;
  }

  reader.close();
  stop_session(&session);
  if (control_connected) {
    std::string reply;
    (void)send_control_command(control_socket_path, "DISCONNECT", &reply);
  }
  if (usb_context != nullptr) libusb_exit(usb_context);
  log::info("消费 daemon 退出");
  return 0;
}
