// SPDX-License-Identifier: MIT
// 使用 libpulse pa_stream 向指定 USB sink 播放 PCM 数据。
// 流带 PA_STREAM_DONT_MOVE，默认 sink 变化时不会被 PulseAudio 移动。
#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>

#include "audio/audio_sink.h"
#include "audio/pulse_stream.h"

namespace usbdisplay {
namespace audio {

class PulsePlayer : public AudioSink {
 public:
  PulsePlayer();
  explicit PulsePlayer(std::unique_ptr<PulseStreamDriver> driver);
  ~PulsePlayer() override;
  PulsePlayer(const PulsePlayer&) = delete;
  PulsePlayer& operator=(const PulsePlayer&) = delete;

  bool open(const std::string& device, uint32_t sample_rate,
            uint16_t channels, std::string* err) override;
  bool write(const void* data, size_t bytes, std::string* err) override;
  void close() override;
  bool is_open() const override;
  std::string describe() const override;

 private:
  std::unique_ptr<PulseStreamDriver> driver_;
};

}  // namespace audio
}  // namespace usbdisplay
