// SPDX-License-Identifier: MIT
// usbdisplay_audio - play WAV audio (or a test tone) through the receiver's
// USB audio interface using ALSA (plughw:USBDISPLAY,0 by default).
//
//   usbdisplay_audio --wav tone.wav
//   usbdisplay_audio --tone-hz 440 --seconds 3
//   usbdisplay_audio --device plughw:1,0 --wav music.wav --loop
#include <algorithm>
#include <cmath>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#include "audio/alsa_player.h"
#include "common/log.h"
#include "common/util.h"
#include "proto/wav.h"

namespace {

volatile std::sig_atomic_t g_stop = 0;
void on_signal(int) { g_stop = 1; }

int16_t clamp16(double v) {
  if (v > 32767.0) return 32767;
  if (v < -32768.0) return -32768;
  return static_cast<int16_t>(v);
}

}  // namespace

int main(int argc, char** argv) {
  using namespace usbdisplay;
  std::string wav_path;
  std::string device = "plughw:USBDISPLAY,0";
  uint32_t tone_hz = 0;
  uint32_t rate = 48000;
  uint16_t channels = 2;
  double seconds = 0.0;
  double volume = 0.3;
  bool loop = false;
  for (int i = 1; i < argc; ++i) {
    const std::string arg = argv[i];
    auto next = [&](std::string* out) -> bool {
      if (i + 1 >= argc) return false;
      *out = argv[++i];
      return true;
    };
    if (arg == "--wav") {
      if (!next(&wav_path)) return 2;
    } else if (arg == "--device") {
      if (!next(&device)) return 2;
    } else if (arg == "--tone-hz") {
      std::string v;
      if (!next(&v)) return 2;
      tone_hz = static_cast<uint32_t>(std::strtoul(v.c_str(), nullptr, 0));
    } else if (arg == "--rate") {
      std::string v;
      if (!next(&v)) return 2;
      rate = static_cast<uint32_t>(std::strtoul(v.c_str(), nullptr, 0));
    } else if (arg == "--channels") {
      std::string v;
      if (!next(&v)) return 2;
      channels = static_cast<uint16_t>(std::strtoul(v.c_str(), nullptr, 0));
    } else if (arg == "--seconds") {
      std::string v;
      if (!next(&v)) return 2;
      seconds = std::atof(v.c_str());
    } else if (arg == "--volume") {
      std::string v;
      if (!next(&v)) return 2;
      volume = std::atof(v.c_str());
    } else if (arg == "--loop") {
      loop = true;
    } else if (arg == "--help" || arg == "-h") {
      std::printf(
          "usage: usbdisplay_audio [--wav FILE | --tone-hz HZ [--seconds N]]\n"
          "  --device DEV     ALSA PCM (default plughw:USBDISPLAY,0)\n"
          "  --rate N         sample rate for tone / WAV validation (48000)\n"
          "  --channels N     channels for tone (default 2)\n"
          "  --volume F       tone volume 0..1 (default 0.3)\n"
          "  --loop           repeat the WAV until SIGINT\n");
      return 0;
    } else {
      std::fprintf(stderr, "unknown argument: %s\n", arg.c_str());
      return 2;
    }
  }
  std::signal(SIGINT, on_signal);
  std::signal(SIGTERM, on_signal);

  std::vector<int16_t> samples;
  std::vector<uint8_t> wav_bytes;
  std::string err;
  if (!wav_path.empty()) {
    if (!read_file(wav_path, &wav_bytes, &err)) {
      std::fprintf(stderr, "%s\n", err.c_str());
      return 1;
    }
    proto::wav::WavInfo info;
    if (!proto::wav::parse(wav_bytes.data(), wav_bytes.size(), &info, &err) ||
        !proto::wav::to_s16le(info, &samples, &err)) {
      std::fprintf(stderr, "%s\n", err.c_str());
      return 1;
    }
    rate = info.sample_rate;
    channels = info.channels;
    if (seconds <= 0.0) {
      seconds = static_cast<double>(samples.size() / channels) / rate;
    }
  } else if (tone_hz > 0) {
    if (seconds <= 0.0) seconds = 3.0;
    const size_t frames = static_cast<size_t>(seconds * rate);
    samples.resize(frames * channels);
    for (size_t f = 0; f < frames; ++f) {
      const double t = static_cast<double>(f) / rate;
      const double value =
          volume * 32767.0 * std::sin(2.0 * 3.14159265358979323846 * tone_hz * t);
      for (uint16_t c = 0; c < channels; ++c) {
        samples[f * channels + c] = clamp16(value);
      }
    }
  } else {
    std::fprintf(stderr, "need --wav FILE or --tone-hz HZ\n");
    return 2;
  }

  audio::AlsaPlayer player;
  if (!player.open(device, rate, channels, &err)) {
    std::fprintf(stderr, "alsa open failed: %s\n", err.c_str());
    return 1;
  }
  std::printf("playing %.2fs %uHz/%uch via %s\n", seconds, rate, channels,
              device.c_str());
  const size_t frame_bytes = static_cast<size_t>(channels) * 2;
  const size_t chunk_frames = static_cast<size_t>(rate) / 50;  // 20 ms
  const size_t chunk_bytes = chunk_frames * frame_bytes;
  const uint8_t* data = reinterpret_cast<const uint8_t*>(samples.data());
  const size_t total_bytes = samples.size() * sizeof(int16_t);
  const uint64_t start_us = mono_us();
  size_t offset = 0;
  while (!g_stop) {
    if (offset >= total_bytes) {
      if (!loop) break;
      offset = 0;
    }
    const size_t n = std::min(chunk_bytes, total_bytes - offset);
    if (!player.write(data + offset, n, &err)) {
      std::fprintf(stderr, "write: %s\n", err.c_str());
      break;
    }
    offset += n;
    if (seconds > 0.0 && (mono_us() - start_us) / 1e6 > seconds + 1.0) break;
  }
  player.close();
  std::printf("done\n");
  return 0;
}
