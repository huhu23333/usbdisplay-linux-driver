// SPDX-License-Identifier: MIT
// 音频输出后端选择：优先 PulseAudio 的 USB sink，失败时回退 ALSA。
#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <string>

#include "audio/audio_sink.h"
#include "audio/pulse_discovery.h"

namespace usbdisplay {
namespace audio {

// 根据安装模式选择后端：PulseAudio 能看到 USB 声卡时用 pulse，否则用 ALSA。
// requested 为 auto 时按环境选择，为 alsa 或 pulse 时直接覆盖。
std::string choose_audio_backend(const std::string& requested,
                                 bool pulse_available, bool usb_card_visible,
                                 const std::string& usb_sink);

struct AudioOptions {
  std::string backend = "alsa";  // 默认 ALSA 直写，pulse 为显式备选
  std::string pulse_sink;        // 指定 PulseAudio sink，留空时自动发现
  std::string alsa_device = "plughw:USBDISPLAY,0";
  uint32_t sample_rate = 48000;
  uint16_t channels = 2;
  bool pulse_available = false;
};

struct AudioOutput {
  std::unique_ptr<AudioSink> sink;
  std::string backend;       // pulse 或 alsa
  std::string device;        // 实际使用的设备名称
  std::string default_sink;  // 启动时读取的 PulseAudio 默认 sink
  std::string error;         // 无可用输出时的错误说明
};

using AudioSinkCreator = std::function<std::unique_ptr<AudioSink>(
    const std::string& device, uint32_t sample_rate, uint16_t channels,
    bool pulse, std::string* err)>;

// 默认创建 PulsePlayer 或 AlsaPlayer。
std::unique_ptr<AudioSink> make_audio_sink(const std::string& device,
                                           uint32_t sample_rate,
                                           uint16_t channels, bool pulse,
                                           std::string* err);

AudioOutput create_audio_output(const AudioOptions& options,
                                PulseDiscovery* discovery,
                                const AudioSinkCreator& creator);

}  // namespace audio
}  // namespace usbdisplay
