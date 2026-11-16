// SPDX-License-Identifier: MIT
// 仅音频投屏：与视频工具使用同一 Controller 会话，完成控制握手后推送 WAV 或测试音。
#include <algorithm>
#include <chrono>
#include <cmath>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <thread>
#include <vector>

#include "common/log.h"
#include "common/util.h"
#include "proto/wav.h"
#include "usbdisplay/device.h"
#include "usbdisplay/protocol.h"

namespace {

volatile std::sig_atomic_t g_stop = 0;
void on_signal(int) { g_stop = 1; }

int16_t clamp16(double v) {
  if (v > 32767.0) return 32767;
  if (v < -32768.0) return -32768;
  return static_cast<int16_t>(v);
}

bool parse_trace(const std::string& text, usbdisplay::TraceLevel* out) {
  static const char* names[] = {"off", "error", "warn", "info", "debug", "wire"};
  for (int i = 0; i < 6; ++i) {
    if (text == names[i]) {
      *out = static_cast<usbdisplay::TraceLevel>(i);
      return true;
    }
  }
  return false;
}

void sleep_until_us(uint64_t target_us) {
  for (;;) {
    const uint64_t now = usbdisplay::mono_us();
    if (target_us <= now) return;
    const uint64_t remaining = target_us - now;
    if (remaining <= 1500) {
      std::this_thread::sleep_for(std::chrono::microseconds(remaining));
      return;
    }
    std::this_thread::sleep_for(std::chrono::microseconds(remaining - 800));
  }
}

}  // namespace

int main(int argc, char** argv) {
  using namespace usbdisplay;
  std::string wav_path;
  std::string alsa_device = "plughw:USBDISPLAY,0";
  std::string audio_backend = "auto";
  std::string pulse_sink;
  std::string hidraw;
  uint16_t vid = kDefaultVid;
  uint16_t pid = kDefaultPid;
  uint32_t tone_hz = 0;
  uint32_t rate = 48000;
  uint16_t channels = 2;
  double seconds = 0.0;
  double volume = 0.3;
  bool loop = false;
  bool no_handshake = false;
  bool restore_default_sink = false;
  TraceLevel trace = TraceLevel::Warn;

  for (int i = 1; i < argc; ++i) {
    const std::string arg = argv[i];
    auto next = [&](std::string* out) -> bool {
      if (i + 1 >= argc) return false;
      *out = argv[++i];
      return true;
    };
    if (arg == "--wav") {
      if (!next(&wav_path)) return 2;
    } else if (arg == "--device" || arg == "--alsa-device") {
      if (!next(&alsa_device)) return 2;
    } else if (arg == "--audio-backend") {
      if (!next(&audio_backend)) return 2;
    } else if (arg == "--pulse-sink") {
      if (!next(&pulse_sink)) return 2;
    } else if (arg == "--hidraw") {
      if (!next(&hidraw)) return 2;
    } else if (arg == "--vid") {
      std::string v;
      if (!next(&v)) return 2;
      vid = static_cast<uint16_t>(std::strtoul(v.c_str(), nullptr, 0));
    } else if (arg == "--pid") {
      std::string v;
      if (!next(&v)) return 2;
      pid = static_cast<uint16_t>(std::strtoul(v.c_str(), nullptr, 0));
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
    } else if (arg == "--no-handshake") {
      no_handshake = true;
    } else if (arg == "--restore-default-sink") {
      restore_default_sink = true;
    } else if (arg == "--trace") {
      std::string v;
      if (!next(&v) || !parse_trace(v, &trace)) return 2;
    } else if (arg == "--help" || arg == "-h") {
      std::printf(
          "usage: usbdisplay_audio [--wav FILE | --tone-hz HZ [--seconds N]]\n"
          "  --alsa-device DEV   ALSA PCM (default plughw:USBDISPLAY,0)\n"
          "  --audio-backend M   auto、alsa 或 pulse（默认 auto）\n"
          "  --pulse-sink NAME   指定 PulseAudio sink\n"
          "  --rate N            tone sample rate (48000)\n"
          "  --channels N        tone channels (default 2)\n"
          "  --volume F          tone volume 0..1 (default 0.3)\n"
          "  --loop              repeat the WAV until SIGINT\n"
          "  --no-handshake      skip the verified JSON handshake\n");
      return 0;
    } else {
      std::fprintf(stderr, "unknown argument: %s\n", arg.c_str());
      return 2;
    }
  }
  log::set_level(trace);
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
      const double value = volume * 32767.0 *
                           std::sin(2.0 * 3.14159265358979323846 * tone_hz * t);
      for (uint16_t c = 0; c < channels; ++c) {
        samples[f * channels + c] = clamp16(value);
      }
    }
  } else {
    std::fprintf(stderr, "need --wav FILE or --tone-hz HZ\n");
    return 2;
  }

  DeviceSelector selector = DeviceSelector::usb(vid, pid);
  selector.media_transport = MediaTransport::UsbHid;
  selector.hidraw_path = hidraw;
  selector.alsa_device = alsa_device;
  selector.audio_backend = audio_backend;
  selector.pulse_sink = pulse_sink;
  selector.restore_default_sink = restore_default_sink;
  selector.enable_audio = true;
  selector.require_audio = true;
  selector.disable_video = true;
  selector.send_handshake = !no_handshake;
  selector.require_handshake = !no_handshake;

  AudioConfig audio_config;
  audio_config.sample_rate = rate;
  audio_config.channels = channels;
  audio_config.format = AudioFormat::PCM_S16LE;
  audio_config.frame_ms = 20;
  VideoConfig video_config;
  video_config.width = 0;
  video_config.height = 0;
  video_config.fps = 0;
  video_config.bitrate_bps = 0;

  Controller controller;
  controller.set_trace(trace);
  if (!controller.open(selector, &err)) {
    std::fprintf(stderr, "open failed: %s\n", err.c_str());
    return 1;
  }
  if (!controller.configure(video_config, audio_config, &err)) {
    std::fprintf(stderr, "configure failed: %s\n", err.c_str());
    controller.close();
    return 1;
  }
  if (!controller.start(&err)) {
    std::fprintf(stderr, "start failed: %s\n", err.c_str());
    controller.close();
    return 1;
  }
  std::printf("audio-only: 设备在没有视频输入时可能不输出音频\n");

  const size_t frame_samples = static_cast<size_t>(rate) * 20 / 1000 * channels;
  const size_t total_chunks =
      (samples.size() + frame_samples - 1) / frame_samples;
  const uint64_t start_us = mono_us();
  uint64_t chunk_index = 0;
  uint64_t chunks_sent = 0;
  while (!g_stop) {
    if (chunk_index >= total_chunks) {
      if (!loop) break;
      chunk_index = 0;
    }
    const uint64_t target_us = start_us + chunks_sent * 20000ULL;
    sleep_until_us(target_us);
    const size_t offset = chunk_index * frame_samples;
    const size_t count = std::min(frame_samples, samples.size() - offset);
    if (count == 0) break;
    if (!controller.push_audio(samples.data() + offset,
                               count * sizeof(int16_t), chunks_sent * 20000ULL,
                               &err)) {
      std::fprintf(stderr, "push_audio failed: %s\n", err.c_str());
      break;
    }
    ++chunk_index;
    ++chunks_sent;
  }
  controller.stop();
  controller.close();
  std::printf("done\n");
  return 0;
}
