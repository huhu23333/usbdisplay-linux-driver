// SPDX-License-Identifier: MIT
#include "audio/audio_backend.h"

#include "audio/alsa_player.h"
#include "audio/pulse_player.h"
#include "common/log.h"

namespace usbdisplay {
namespace audio {

std::string choose_audio_backend(const std::string& requested,
                                 bool pulse_available, bool usb_card_visible,
                                 const std::string& usb_sink) {
  (void)usb_sink;
  if (requested == "pulse") return "pulse";
  if (requested == "alsa") return "alsa";
  if (pulse_available && usb_card_visible) {
    return "pulse";
  }
  return "alsa";
}

std::unique_ptr<AudioSink> make_audio_sink(const std::string& device,
                                           uint32_t sample_rate,
                                           uint16_t channels, bool pulse,
                                           std::string* err) {
  std::unique_ptr<AudioSink> sink;
  if (pulse) {
    sink.reset(new PulsePlayer());
  } else {
    sink.reset(new AlsaPlayer());
  }
  if (!sink->open(device, sample_rate, channels, err)) {
    sink.reset();
  }
  return sink;
}

AudioOutput create_audio_output(const AudioOptions& options,
                                PulseDiscovery* discovery,
                                const AudioSinkCreator& creator) {
  AudioOutput output;
  std::string default_err;
  if (discovery != nullptr) {
    discovery->get_default_sink(&output.default_sink, &default_err);
  }
  auto try_alsa = [&]() -> bool {
    std::string err;
    output.sink = creator(options.alsa_device, options.sample_rate,
                          options.channels, false, &err);
    if (output.sink) {
      output.backend = "alsa";
      output.device = options.alsa_device;
      return true;
    }
    output.error = "ALSA 输出打开失败：" + err;
    return false;
  };
  if (options.backend == "alsa") {
    try_alsa();
    return output;
  }
  auto try_pulse = [&]() -> bool {
    if (!options.pulse_available) {
      output.error = "构建时未启用 PulseAudio 支持";
      return false;
    }
    if (discovery == nullptr) {
      output.error = "PulseAudio 设备发现不可用";
      return false;
    }
    std::string sink_name = options.pulse_sink;
    if (sink_name.empty()) {
      std::vector<PulseSinkInfo> sinks;
      std::string err;
      if (!discovery->list_sinks(&sinks, &err)) {
        output.error = "PulseAudio sink 查询失败：" + err;
        return false;
      }
      sink_name = select_usb_sink(sinks);
    }
    if (sink_name.empty()) {
      output.error = "未找到 USB 显示设备的 PulseAudio sink";
      return false;
    }
    std::string err;
    output.sink = creator(sink_name, options.sample_rate, options.channels,
                          true, &err);
    if (!output.sink) {
      output.error = "PulseAudio 输出打开失败：" + err;
      return false;
    }
    output.backend = "pulse";
    output.device = sink_name;
    return true;
  };
  if (try_pulse()) return output;
  if (options.backend == "pulse") return output;
  log::warn(output.error + "，回退 ALSA");
  try_alsa();
  return output;
}

}  // namespace audio
}  // namespace usbdisplay
