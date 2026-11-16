// SPDX-License-Identifier: MIT
// 音频播放接口，便于替换为测试实现。
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

namespace usbdisplay {
namespace audio {

class AudioSink {
 public:
  virtual ~AudioSink() = default;
  virtual bool open(const std::string& device, uint32_t sample_rate,
                    uint16_t channels, std::string* err) = 0;
  virtual bool write(const void* data, size_t bytes, std::string* err) = 0;
  virtual void close() = 0;
  virtual bool is_open() const = 0;
  virtual std::string describe() const = 0;
};

}  // namespace audio
}  // namespace usbdisplay
