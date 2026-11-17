// SPDX-License-Identifier: MIT
#include "evdi/state.h"

#include <algorithm>

namespace usbdisplay {
namespace evdi {

void UsbSessionStateMachine::set_usb_present(bool present, uint64_t now_ms) {
  if (present == usb_present_) return;
  usb_present_ = present;
  if (!present) {
    state_ = UsbSessionState::Idle;
    failures_ = 0;
    next_attempt_ms_ = now_ms;
  } else if (state_ != UsbSessionState::Streaming) {
    state_ = UsbSessionState::Idle;
    next_attempt_ms_ = now_ms;
  }
}

bool UsbSessionStateMachine::usb_present() const { return usb_present_; }
UsbSessionState UsbSessionStateMachine::state() const { return state_; }

bool UsbSessionStateMachine::should_attempt(uint64_t now_ms) const {
  return usb_present_ && state_ != UsbSessionState::Streaming &&
         now_ms >= next_attempt_ms_;
}

void UsbSessionStateMachine::on_start_success() {
  state_ = UsbSessionState::Streaming;
  failures_ = 0;
  next_attempt_ms_ = 0;
}

void UsbSessionStateMachine::on_start_failure(uint64_t now_ms) {
  state_ = UsbSessionState::Backoff;
  ++failures_;
  next_attempt_ms_ = now_ms + static_cast<uint64_t>(backoff_ms());
}

void UsbSessionStateMachine::on_usb_removed() {
  usb_present_ = false;
  state_ = UsbSessionState::Idle;
  failures_ = 0;
  next_attempt_ms_ = 0;
}

int UsbSessionStateMachine::failures() const { return failures_; }

int UsbSessionStateMachine::backoff_ms() const {
  return std::min(5000, 2000 + (failures_ > 0 ? failures_ - 1 : 0) * 500);
}

uint64_t UsbSessionStateMachine::next_attempt_ms() const {
  return next_attempt_ms_;
}

}  // namespace evdi
}  // namespace usbdisplay
