// SPDX-License-Identifier: MIT
// evdi 低层驱动接口，便于用 fake 注入请求/事件故障；默认真实 libevdi。
#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace usbdisplay {
namespace evdi {

struct EvdiModeInfo {
  int width = 0;
  int height = 0;
  int refresh_rate = 0;
  int bits_per_pixel = 0;
  uint32_t pixel_format = 0;
};

struct EvdiRect {
  int x1 = 0;
  int y1 = 0;
  int x2 = 0;
  int y2 = 0;
};

struct EvdiBufferDesc {
  int id = 0;
  void* data = nullptr;
  int width = 0;
  int height = 0;
  int stride = 0;
  EvdiRect* rects = nullptr;
  int rect_count = 0;
};

struct EvdiEventCallbacks {
  std::function<void(const EvdiModeInfo&)> mode_changed;
  std::function<void(int buffer_id)> update_ready;
  std::function<void(int dpms_mode)> dpms_changed;
  std::function<void(int state)> crtc_state;
};

class EvdiDriver {
 public:
  virtual ~EvdiDriver() = default;
  virtual bool open(int card_index, std::string* err) = 0;
  virtual void close() = 0;
  virtual bool connect(const std::vector<uint8_t>& edid, int fps,
                       std::string* err) = 0;
  virtual void disconnect() = 0;
  virtual void enable_cursor(bool enable) = 0;
  virtual void set_callbacks(const EvdiEventCallbacks& callbacks) = 0;
  virtual bool register_buffer(const EvdiBufferDesc& buffer,
                               std::string* err) = 0;
  virtual void unregister_buffer(int buffer_id) = 0;
  // 返回 false 且 err 非空表示驱动错误；immediate 表示当前已有 dirty 可立即抓取。
  virtual bool request_update(int buffer_id, bool* immediate,
                              std::string* err) = 0;
  // 返回矩形数；-1 表示 grab 失败。
  virtual int grab_pixels(int buffer_id, std::vector<EvdiRect>* rects,
                          std::string* err) = 0;
  virtual int event_fd() const = 0;
  // event_ready=false 表示超时；返回 false 表示 EOF/HUP/读取错误。
  virtual bool wait_events(int timeout_ms, bool* event_ready,
                           std::string* err) = 0;
};

std::unique_ptr<EvdiDriver> make_libevdi_driver();

}  // namespace evdi
}  // namespace usbdisplay
