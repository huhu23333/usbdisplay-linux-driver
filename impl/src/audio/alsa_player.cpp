// SPDX-License-Identifier: MIT
#include "audio/alsa_player.h"

#include <cerrno>

#include "common/log.h"
#include "common/util.h"

#ifdef USBDISPLAY_HAVE_ALSA
#include <alsa/asoundlib.h>
#endif

namespace usbdisplay {
namespace audio {

AlsaPlayer::AlsaPlayer() = default;
AlsaPlayer::~AlsaPlayer() { close(); }

#ifdef USBDISPLAY_HAVE_ALSA

bool AlsaPlayer::open(const std::string& device, uint32_t sample_rate,
                      uint16_t channels, std::string* err) {
  close();
  if (device.empty()) {
    if (err) *err = "no ALSA device configured";
    return false;
  }
  snd_pcm_t* pcm = nullptr;
  const int rc =
      ::snd_pcm_open(&pcm, device.c_str(), SND_PCM_STREAM_PLAYBACK, 0);
  if (rc < 0) {
    if (err) {
      *err = "snd_pcm_open(" + device + "): " + ::snd_strerror(rc);
    }
    return false;
  }
  const unsigned int rate = sample_rate;
  const int prc = ::snd_pcm_set_params(
      pcm, SND_PCM_FORMAT_S16_LE, SND_PCM_ACCESS_RW_INTERLEAVED, channels, rate,
      1, 100000);  // 100 ms latency, allow resampling
  if (prc < 0) {
    if (err) {
      *err = "snd_pcm_set_params: " + std::string(::snd_strerror(prc));
    }
    ::snd_pcm_close(pcm);
    return false;
  }
  // 使用非阻塞模式，避免 ALSA 缓冲满时播放线程无限期阻塞。
  ::snd_pcm_nonblock(pcm, 1);
  pcm_ = pcm;
  device_ = device;
  sample_rate_ = sample_rate;
  channels_ = channels;
  bytes_written_ = 0;
  log::info("alsa: opened " + device + " " + std::to_string(sample_rate) +
            "Hz/" + std::to_string(channels) + "ch s16le");
  return true;
}

bool AlsaPlayer::write(const void* data, size_t bytes, std::string* err) {
  if (pcm_ == nullptr) {
    if (err) *err = "alsa: not open";
    return false;
  }
  const size_t frame_bytes = static_cast<size_t>(channels_) * 2;
  if (frame_bytes == 0 || bytes % frame_bytes != 0) {
    if (err) *err = "alsa: buffer not frame aligned";
    return false;
  }
  const size_t frames = bytes / frame_bytes;
  const uint8_t* p = static_cast<const uint8_t*>(data);
  size_t written = 0;
  const uint64_t deadline = mono_ms() + 500;
  while (written < frames) {
    const snd_pcm_sframes_t n = ::snd_pcm_writei(
        static_cast<snd_pcm_t*>(pcm_),
        p + written * frame_bytes, frames - written);
    if (n < 0) {
      if (n == -EAGAIN) {
        if (mono_ms() >= deadline) {
          if (err) *err = "ALSA 写入超时";
          return false;
        }
        ::snd_pcm_wait(static_cast<snd_pcm_t*>(pcm_), 100);
        continue;
      }
      const int rc = ::snd_pcm_recover(static_cast<snd_pcm_t*>(pcm_),
                                       static_cast<int>(n), 1);
      if (rc < 0) {
        if (err) *err = "snd_pcm_writei: " + std::string(::snd_strerror(rc));
        return false;
      }
      continue;
    }
    written += static_cast<size_t>(n);
  }
  bytes_written_ += bytes;
  return true;
}

void AlsaPlayer::close() {
  if (pcm_ != nullptr) {
    ::snd_pcm_drop(static_cast<snd_pcm_t*>(pcm_));
    ::snd_pcm_close(static_cast<snd_pcm_t*>(pcm_));
    pcm_ = nullptr;
  }
  device_.clear();
}

std::string AlsaPlayer::describe() const {
  if (pcm_ == nullptr) return "alsa:closed";
  return "alsa:" + device_ + " " + std::to_string(sample_rate_) + "Hz/" +
         std::to_string(channels_) + "ch";
}

#else  // !USBDISPLAY_HAVE_ALSA

bool AlsaPlayer::open(const std::string&, uint32_t, uint16_t,
                      std::string* err) {
  if (err) *err = "ALSA support was not compiled in (install libasound2-dev)";
  return false;
}
bool AlsaPlayer::write(const void*, size_t, std::string* err) {
  if (err) *err = "ALSA support was not compiled in";
  return false;
}
void AlsaPlayer::close() {}
std::string AlsaPlayer::describe() const { return "alsa:unavailable"; }

#endif

}  // namespace audio
}  // namespace usbdisplay
