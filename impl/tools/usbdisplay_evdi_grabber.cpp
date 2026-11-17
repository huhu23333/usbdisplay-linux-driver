// SPDX-License-Identifier: MIT
// evdi 专用 grabber：只抓帧并发布到共享内存，不编码、不访问 USB 视频接口。
//
// 该程序只能由 usbdisplay-evdi.service 启动；服务 ExecStartPre 先创建/复用
// 专用 evdi card，随后启动本进程。正常停止时先 evdi_disconnect 再退出。
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits.h>
#include <string>
#include <thread>
#include <vector>

#include "common/log.h"
#include "common/util.h"
#include "evdi/control_server.h"
#include "evdi/edid.h"
#include "evdi/evdi_capture.h"
#include "evdi/frame_lease_server.h"
#include "evdi/marker.h"
#include "evdi/shared_frame.h"
#include "usbdisplay/protocol.h"

namespace {

volatile std::sig_atomic_t g_stop = 0;
void on_signal(int) { g_stop = 1; }

constexpr const char* kDefaultMarker = "/run/usbdisplay/evdi-card";
constexpr uint64_t kGrabWatchdogMs = 30000;

void usage() {
  std::printf(
      "usage: usbdisplay_evdi_grabber [options]\n"
      "  --marker PATH    evdi marker（默认 /run/usbdisplay/evdi-card）\n"
      "  --shm-path PATH  共享帧路径（默认 /dev/shm/usbdisplay-frames）\n"
      "  --socket PATH    租约 socket（默认 /run/usbdisplay/frames.sock）\n"
      "  --control PATH   控制 socket（默认 /run/usbdisplay/control.sock）\n"
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

void notify_ready() {
  const char* notify_socket = std::getenv("NOTIFY_SOCKET");
  if (notify_socket == nullptr || notify_socket[0] == '\0') return;
  const int fd = ::socket(AF_UNIX, SOCK_DGRAM | SOCK_CLOEXEC, 0);
  if (fd < 0) return;
  sockaddr_un address{};
  address.sun_family = AF_UNIX;
  size_t path_len = std::strlen(notify_socket);
  if (notify_socket[0] == '@') {
    address.sun_path[0] = '\0';
    path_len = std::min(path_len - 1, sizeof(address.sun_path) - 1);
    std::memcpy(address.sun_path + 1, notify_socket + 1, path_len);
  } else {
    path_len = std::min(path_len, sizeof(address.sun_path) - 1);
    std::memcpy(address.sun_path, notify_socket, path_len);
  }
  const socklen_t length =
      static_cast<socklen_t>(offsetof(sockaddr_un, sun_path) + path_len +
                             (notify_socket[0] == '@' ? 1 : 0));
  const char message[] = "READY=1";
  (void)::sendto(fd, message, sizeof(message) - 1, MSG_NOSIGNAL,
                 reinterpret_cast<const sockaddr*>(&address), length);
  ::close(fd);
}

void sleep_interruptible(int milliseconds) {
  for (int elapsed = 0; elapsed < milliseconds && !g_stop; elapsed += 50) {
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
  }
}

bool file_exists(const std::string& path) {
  return ::access(path.c_str(), F_OK) == 0;
}

std::string basename_of(const std::string& path) {
  const size_t slash = path.find_last_of('/');
  return slash == std::string::npos ? path : path.substr(slash + 1);
}

struct MarkerIdentity {
  std::string card;
  std::string connector;
  std::string platform;
  std::string device;
};

bool realpath_of(const std::string& path, std::string* out) {
  char buffer[PATH_MAX] = {0};
  if (::realpath(path.c_str(), buffer) == nullptr) return false;
  *out = buffer;
  return true;
}

bool validate_marker(const usbdisplay::evdi::Marker& marker,
                     MarkerIdentity* identity, std::string* err) {
  if (!marker.valid) {
    if (err) *err = marker.error;
    return false;
  }
  const std::string card_dir = "/sys/class/drm/" + marker.card_name;
  const std::string driver =
      "/sys/class/drm/" + marker.card_name + "/device/driver";
  char driver_path[4096] = {0};
  if (::readlink(driver.c_str(), driver_path, sizeof(driver_path) - 1) <= 0 ||
      basename_of(driver_path) != "evdi") {
    if (err) *err = "marker 指定的 card driver 不是 evdi";
    return false;
  }
  if (!file_exists(card_dir)) {
    if (err) *err = "marker card sysfs 不存在";
    return false;
  }
  if (!file_exists(card_dir + "-" + marker.connector)) {
    if (err) *err = "marker connector 不存在";
    return false;
  }
  MarkerIdentity current;
  current.card = marker.card;
  current.connector = marker.connector;
  if (!realpath_of(marker.platform, &current.platform) ||
      !realpath_of(card_dir + "/device", &current.device)) {
    if (err) *err = "marker platform 或 card device 无法解析";
    return false;
  }
  if (current.platform != current.device) {
    if (err) *err = "marker platform 与 card device realpath 不一致";
    return false;
  }
  if (identity != nullptr) *identity = current;
  return true;
}

}  // namespace

int main(int argc, char** argv) {
  using namespace usbdisplay;
  std::string marker_path = kDefaultMarker;
  std::string shm_path = evdi::kDefaultSharedFramePath;
  std::string socket_path = evdi::kDefaultLeaseSocketPath;
  std::string control_path = "/run/usbdisplay/control.sock";
  TraceLevel trace = TraceLevel::Warn;
  for (int i = 1; i < argc; ++i) {
    const std::string arg = argv[i];
    auto next = [&](std::string* out) -> bool {
      if (i + 1 >= argc) return false;
      *out = argv[++i];
      return true;
    };
    if (arg == "--marker") {
      if (!next(&marker_path)) return 2;
    } else if (arg == "--shm-path") {
      if (!next(&shm_path)) return 2;
    } else if (arg == "--socket") {
      if (!next(&socket_path)) return 2;
    } else if (arg == "--control") {
      if (!next(&control_path)) return 2;
    } else if (arg == "--trace") {
      std::string value;
      if (!next(&value) || !parse_trace(value, &trace)) return 2;
    } else if (arg == "--help" || arg == "-h") {
      usage();
      return 0;
    } else {
      std::fprintf(stderr, "未知参数：%s\n", arg.c_str());
      usage();
      return 2;
    }
  }
  if (std::getenv("INVOCATION_ID") == nullptr) {
    std::fprintf(stderr,
                 "usbdisplay_evdi_grabber 只能由 usbdisplay-evdi.service 启动；"
                 "请使用 systemctl start usbdisplay-evdi.service\n");
    return 1;
  }
  log::set_level(trace);
  std::signal(SIGINT, on_signal);
  std::signal(SIGTERM, on_signal);

  std::string last_marker_error;
  evdi::Marker marker;
  MarkerIdentity identity;
  while (!g_stop) {
    marker = evdi::load_marker(marker_path);
    std::string marker_err;
    if (validate_marker(marker, &identity, &marker_err)) break;
    if (marker_err != last_marker_error) {
      log::warn("等待有效 evdi marker：" + marker_err);
      last_marker_error = marker_err;
    }
    sleep_interruptible(1000);
  }

  evdi::SharedFrameWriter writer;
  evdi::EvdiCapture capture;
  if (!g_stop) {
    if (!writer.open(shm_path, &last_marker_error)) {
      log::warn("创建共享帧失败：" + last_marker_error);
      return 1;
    }
    if (!capture.open(marker.card_index, &last_marker_error)) {
      log::warn("打开 evdi card 失败：" + last_marker_error);
      writer.close();
      return 1;
    }
  }
  capture.set_health_timeout_ms(20000);
  capture.set_initial_force_grab(true);
  evdi::FrameLeaseServer lease_server(&writer);
  if (!g_stop && !lease_server.listen(socket_path, &last_marker_error)) {
    log::warn("创建租约 socket 失败：" + last_marker_error);
    capture.close();
    writer.close();
    return 1;
  }

  // grabber 只依赖 evdi；USB 用户态驱动单独自启动，通过控制 socket 调用 CONNECT。
  bool display_connected = false;
  bool first_grab_seen = false;
  bool publish_blocked = false;
  uint64_t last_grab_ms = 0;
  uint64_t last_publish_success_ms = 0;
  uint64_t last_publish_attempt_ms = 0;
  uint64_t last_identity_check_ms = 0;
  uint64_t last_force_grab_ms = 0;
  uint64_t last_force_log_ms = 0;
  uint64_t last_drop_log_ms = 0;
  bool healthy = true;
  capture.set_frame_handler([&](const uint8_t* data, int, int, int stride) {
    if (!display_connected) return;
    last_grab_ms = mono_ms();
    first_grab_seen = true;
    std::string publish_err;
    const uint64_t now = mono_ms();
    last_publish_attempt_ms = now;
    if (!writer.publish(data, stride, &publish_err)) {
      if (now - last_drop_log_ms > 5000) {
        log::warn("共享帧发布丢弃：" + publish_err);
        last_drop_log_ms = now;
      }
      if (last_publish_success_ms != 0 &&
          now - last_publish_success_ms > 3000) {
        publish_blocked = true;
      }
    } else {
      last_publish_success_ms = now;
      publish_blocked = false;
    }
  });
  evdi::ControlServer control_server(
      [&](int requested_fps, std::string* connect_err) {
        if (requested_fps != 60 && requested_fps != 120) {
          if (connect_err) *connect_err = "unsupported-fps";
          return false;
        }
        if (display_connected) return true;
        // 单次 EDID 同时暴露 60/120，pixel_per_second 固定按 120 申请。
        const std::vector<uint8_t> edid = evdi::make_edid_1920x1080();
        if (!capture.connect(edid, 120, connect_err)) return false;
        display_connected = true;
        first_grab_seen = false;
        last_grab_ms = mono_ms();
        last_publish_success_ms = 0;
        publish_blocked = false;
        log::info("控制 CONNECT：1080p60/120 双模式已连接");
        return true;
      },
      [&]() {
        if (!display_connected) return;
        capture.disconnect();
        writer.invalidate();
        display_connected = false;
        first_grab_seen = false;
        last_grab_ms = 0;
        publish_blocked = false;
        log::info("控制 DISCONNECT：停止抓帧");
      },
      [&]() {
        return std::string("OK connected=") +
               (display_connected ? "1" : "0") +
               " width=1920 height=1080 modes=60,120";
      });
  if (!control_server.listen(control_path, &last_marker_error)) {
    log::warn("创建控制 socket 失败：" + last_marker_error);
    lease_server.close();
    capture.close();
    writer.close();
    return 1;
  }
  notify_ready();

  while (!g_stop) {
    control_server.poll();
    lease_server.poll();
    const uint64_t now = mono_ms();
    if (now - last_identity_check_ms >= 1000) {
      last_identity_check_ms = now;
      evdi::Marker current_marker = evdi::load_marker(marker_path);
      MarkerIdentity current_identity;
      std::string identity_err;
      if (!validate_marker(current_marker, &current_identity, &identity_err) ||
          current_identity.card != identity.card ||
          current_identity.connector != identity.connector ||
          current_identity.platform != identity.platform ||
          current_identity.device != identity.device) {
        log::warn("marker/card 身份变化或失效，主动断开并退出：" + identity_err);
        healthy = false;
        break;
      }
    }
    if (!display_connected) {
      writer.heartbeat();
      sleep_interruptible(20);
      continue;
    }
    if (!capture.poll(10)) {
      std::string poll_reason;
      capture.healthy(&poll_reason);
      log::warn("evdi 事件循环失败；断开虚拟显示但保持服务与控制接口：" +
                poll_reason);
      capture.disconnect();
      writer.invalidate();
      display_connected = false;
      first_grab_seen = false;
      last_grab_ms = 0;
      continue;
    }
    if (!first_grab_seen && last_grab_ms != 0 &&
        mono_ms() - last_grab_ms > kGrabWatchdogMs) {
      log::warn("连接后首帧超时，主动断开并退出让 systemd 重启");
      healthy = false;
      break;
    }
    if (mono_ms() - last_force_grab_ms >= 1000 &&
        mono_ms() - last_grab_ms >= 1000) {
      last_force_grab_ms = mono_ms();
      if (last_force_log_ms == 0 ||
          mono_ms() - last_force_log_ms >= 5000) {
        log::warn("1 秒无 grab 事件，主动补抓一次");
        last_force_log_ms = mono_ms();
      }
      capture.force_grab();
    }
    if (publish_blocked) {
      log::warn("共享帧槽位被长时间占用，退出让服务重启并重建共享内存");
      healthy = false;
      break;
    }
    if (!capture.healthy()) {
      log::warn("evdi 抓帧状态异常；断开虚拟显示但保持服务");
      capture.disconnect();
      writer.invalidate();
      display_connected = false;
      first_grab_seen = false;
      last_grab_ms = 0;
      continue;
    }
    writer.heartbeat();
  }

  // 正常或异常退出都先断开 evdi，再关闭 card 与共享帧。
  control_server.close();
  lease_server.close();
  capture.disconnect();
  capture.close();
  writer.invalidate();
  writer.close();
  return healthy ? 0 : 1;
}
