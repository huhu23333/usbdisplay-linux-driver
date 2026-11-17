// SPDX-License-Identifier: MIT
// evdi 帧抓取：持有 outstanding 请求/进展健康状态，驱动可注入。
#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "evdi/evdi_driver.h"

namespace usbdisplay {
namespace evdi {

class EvdiCapture {
 public:
  using FrameHandler = std::function<void(const uint8_t* data, int width,
                                          int height, int stride)>;

  EvdiCapture();
  explicit EvdiCapture(std::unique_ptr<EvdiDriver> driver);
  ~EvdiCapture();
  EvdiCapture(const EvdiCapture&) = delete;
  EvdiCapture& operator=(const EvdiCapture&) = delete;

  bool open(int card_index, std::string* err);
  bool connect(const std::vector<uint8_t>& edid, int fps, std::string* err);
  bool connect(const std::vector<uint8_t>& edid, std::string* err);
  void disconnect();
  void close();
  bool poll(int timeout_ms);
  bool force_grab();
  void set_initial_force_grab(bool enable);
  bool connected() const;
  bool is_open() const;
  bool healthy(std::string* reason = nullptr) const;
  bool stalled(uint64_t milliseconds) const;
  uint64_t last_progress_ms() const;
  void set_health_timeout_ms(int timeout_ms);
  void set_frame_handler(FrameHandler handler);
  int width() const;
  int height() const;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace evdi
}  // namespace usbdisplay
