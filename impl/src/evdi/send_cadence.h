// SPDX-License-Identifier: MIT
// USB 发送节拍：固定 60fps，PTS 按发送序号。
#pragma once
#include <cstdint>
namespace usbdisplay { namespace evdi {
class SendCadence {
 public:
  explicit SendCadence(int fps = 60) : fps_(fps > 0 ? fps : 60) {}
  bool due(uint64_t now_ms) const {
    return next_us_ == 0 || now_ms * 1000ULL >= next_us_;
  }
  void advance(uint64_t now_ms) {
    next_us_ = now_ms * 1000ULL +
               static_cast<uint64_t>(1000000 / fps_);
  }
  void reset(uint64_t now_ms) { next_us_ = now_ms * 1000ULL; }
  uint64_t pts_us(uint64_t sent_frames) const {
    return sent_frames * 1000000ULL / static_cast<uint64_t>(fps_);
  }
  int fps() const { return fps_; }
 private:
  int fps_;
  uint64_t next_us_ = 0;
};
}}  // namespace
