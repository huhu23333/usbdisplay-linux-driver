// SPDX-License-Identifier: MIT
// PulseAudio 播放流驱动接口，便于测试注入并固定输出到指定 USB sink。
#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>

namespace usbdisplay {
namespace audio {

struct PulseStreamOpenOptions {
  std::string device;           // 显式指定 USB sink 名称
  uint32_t sample_rate = 0;
  uint16_t channels = 0;
  int target_latency_ms = 100;
  bool dont_move = true;        // 请求 PA_STREAM_DONT_MOVE
};

class PulseStreamDriver {
 public:
  virtual ~PulseStreamDriver() = default;
  virtual bool open(const PulseStreamOpenOptions& options,
                    std::string* err) = 0;
  virtual bool write(const void* data, size_t bytes, std::string* err) = 0;
  virtual void close() = 0;
  virtual bool is_open() const = 0;
  virtual std::string describe() const = 0;
};

std::unique_ptr<PulseStreamDriver> make_libpulse_stream_driver();

}  // namespace audio
}  // namespace usbdisplay
