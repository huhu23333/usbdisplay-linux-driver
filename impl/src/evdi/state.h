// SPDX-License-Identifier: MIT
// USB 插拔与连接重试状态机，独立于真实设备和时钟。
#pragma once

#include <cstdint>

namespace usbdisplay {
namespace evdi {

enum class UsbSessionState {
  Idle,
  Backoff,
  Streaming,
};

class UsbSessionStateMachine {
 public:
  void set_usb_present(bool present, uint64_t now_ms);
  bool usb_present() const;
  UsbSessionState state() const;
  bool should_attempt(uint64_t now_ms) const;
  void on_start_success();
  void on_start_failure(uint64_t now_ms);
  void on_usb_removed();
  int failures() const;
  int backoff_ms() const;
  uint64_t next_attempt_ms() const;

 private:
  bool usb_present_ = false;
  UsbSessionState state_ = UsbSessionState::Idle;
  int failures_ = 0;
  uint64_t next_attempt_ms_ = 0;
};

}  // namespace evdi
}  // namespace usbdisplay
