// SPDX-License-Identifier: MIT
// 接收器 USB 音频接口的 ALSA 播放。
// 内核 snd-usb-audio 驱动管理该接口，用户态只需打开 PCM 设备。
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

#include "audio/audio_sink.h"

namespace usbdisplay {
namespace audio {

class AlsaPlayer : public AudioSink {
 public:
  AlsaPlayer();
  ~AlsaPlayer() override;
  AlsaPlayer(const AlsaPlayer&) = delete;
  AlsaPlayer& operator=(const AlsaPlayer&) = delete;

  bool open(const std::string& device, uint32_t sample_rate,
            uint16_t channels, std::string* err) override;
  bool write(const void* data, size_t bytes, std::string* err) override;
  void close() override;
  bool is_open() const override { return pcm_ != nullptr; }
  std::string describe() const override;

 private:
  void* pcm_ = nullptr;
  std::string device_;
  uint32_t sample_rate_ = 0;
  uint16_t channels_ = 0;
  size_t bytes_written_ = 0;
};

}  // namespace audio
}  // namespace usbdisplay
