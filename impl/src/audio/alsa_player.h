// SPDX-License-Identifier: MIT
// ALSA playback for the receiver's USB audio interface (IF2/IF3).
// The kernel snd-usb-audio driver owns the interface, so userspace just opens
// a PCM device (e.g. "plughw:USBDISPLAY,0" or "plughw:1,0").
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

namespace usbdisplay {
namespace audio {

class AlsaPlayer {
 public:
  AlsaPlayer();
  ~AlsaPlayer();
  AlsaPlayer(const AlsaPlayer&) = delete;
  AlsaPlayer& operator=(const AlsaPlayer&) = delete;

  bool open(const std::string& device, uint32_t sample_rate,
            uint16_t channels, std::string* err);
  bool write(const void* data, size_t bytes, std::string* err);
  void close();
  bool is_open() const { return pcm_ != nullptr; }
  std::string describe() const;

 private:
  void* pcm_ = nullptr;
  std::string device_;
  uint32_t sample_rate_ = 0;
  uint16_t channels_ = 0;
  size_t bytes_written_ = 0;
};

}  // namespace audio
}  // namespace usbdisplay
