// SPDX-License-Identifier: MIT
#include <memory>
#include <string>
#include <vector>

#include "audio/audio_backend.h"
#include "audio/pulse_discovery.h"
#include "test_harness.h"

namespace {

class FakeSink : public usbdisplay::audio::AudioSink {
 public:
  bool open(const std::string&, uint32_t, uint16_t, std::string*) override {
    open_ = true;
    return true;
  }
  bool write(const void*, size_t, std::string*) override { return true; }
  void close() override { open_ = false; }
  bool is_open() const override { return open_; }
  std::string describe() const override { return "fake"; }

 private:
  bool open_ = false;
};

class FakeDiscovery : public usbdisplay::audio::PulseDiscovery {
 public:
  std::vector<usbdisplay::audio::PulseSinkInfo> sinks;
  std::string default_name = "内置模拟输出";
  bool fail_list = false;
  int list_calls = 0;
  int default_calls = 0;

  bool list_sinks(std::vector<usbdisplay::audio::PulseSinkInfo>* out,
                  std::string* err) override {
    ++list_calls;
    if (fail_list) {
      if (err) *err = "查询失败";
      return false;
    }
    *out = sinks;
    return true;
  }
  bool get_default_sink(std::string* out, std::string*) override {
    ++default_calls;
    *out = default_name;
    return true;
  }
  bool set_default_sink(const std::string&, std::string*) override { return true; }
};

struct CreatorState {
  int pulse_calls = 0;
  int alsa_calls = 0;
  bool fail_pulse = false;
  std::string last_pulse_device;
  std::string last_alsa_device;
};

usbdisplay::audio::AudioSinkCreator make_creator(CreatorState* state) {
  return [state](const std::string& device, uint32_t, uint16_t, bool pulse,
                 std::string* err) -> std::unique_ptr<usbdisplay::audio::AudioSink> {
    if (pulse) {
      ++state->pulse_calls;
      state->last_pulse_device = device;
      if (state->fail_pulse) {
        if (err) *err = "设备忙";
        return nullptr;
      }
    } else {
      ++state->alsa_calls;
      state->last_alsa_device = device;
    }
    std::unique_ptr<FakeSink> sink(new FakeSink());
    std::string open_err;
    sink->open(device, 48000, 2, &open_err);
    return sink;
  };
}

usbdisplay::audio::AudioOptions make_options(bool pulse_available) {
  usbdisplay::audio::AudioOptions options;
  options.backend = "auto";
  options.sample_rate = 48000;
  options.channels = 2;
  options.pulse_available = pulse_available;
  return options;
}

usbdisplay::audio::PulseSinkInfo make_sink(const std::string& name) {
  usbdisplay::audio::PulseSinkInfo sink;
  sink.name = name;
  return sink;
}

}  // namespace

int main() {
  using namespace usbdisplay::audio;
  {
    // ALSA 接口可独立创建与关闭。
    FakeDiscovery discovery;
    CreatorState state;
    AudioOptions options;
    options.backend = "alsa";
    options.sample_rate = 48000;
    options.channels = 2;
    AudioOutput output =
        create_audio_output(options, &discovery, make_creator(&state));
    CHECK(output.sink != nullptr);
    CHECK(output.backend == "alsa");
    CHECK(output.sink->is_open());
    output.sink->close();
    CHECK(!output.sink->is_open());
  }
  {
    // PulseAudio 接口可独立创建与关闭，不依赖 ALSA 路径。
    FakeDiscovery discovery;
    discovery.sinks = {
        make_sink("alsa_output.usb-Actions_USB_DISPLAY_51d3f01-02.analog-stereo")};
    CreatorState state;
    AudioOptions options = make_options(true);
    options.backend = "pulse";
    AudioOutput output =
        create_audio_output(options, &discovery, make_creator(&state));
    CHECK(output.sink != nullptr);
    CHECK(output.backend == "pulse");
    CHECK(output.sink->is_open());
    output.sink->close();
    CHECK(!output.sink->is_open());
    CHECK(state.alsa_calls == 0);
  }
  {
    // 关闭音频接口不影响独立的视频发送路径。
    FakeSink audio;
    std::string open_err;
    CHECK(audio.open("", 48000, 2, &open_err));
    audio.close();
    int video_frames = 0;
    ++video_frames;
    CHECK(!audio.is_open());
    CHECK(video_frames == 1);
  }
  {
    // 默认后端为 ALSA，PulseAudio 可用时也不创建 Pulse 播放流。
    FakeDiscovery discovery;
    discovery.sinks = {
        make_sink("alsa_output.usb-Actions_USB_DISPLAY_51d3f01-02.analog-stereo")};
    CreatorState state;
    AudioOptions options;
    options.sample_rate = 48000;
    options.channels = 2;
    options.pulse_available = true;
    AudioOutput output =
        create_audio_output(options, &discovery, make_creator(&state));
    CHECK(output.sink != nullptr);
    CHECK(output.backend == "alsa");
    CHECK(state.pulse_calls == 0 && state.alsa_calls == 1);
    CHECK(discovery.list_calls == 0);
  }
  {
    // --audio-backend pulse 分支存在，自动选择 USB sink。
    FakeDiscovery discovery;
    discovery.sinks = {
        make_sink("alsa_output.usb-Actions_USB_DISPLAY_51d3f01-02.analog-stereo")};
    CreatorState state;
    AudioOptions options = make_options(true);
    options.backend = "pulse";
    AudioOutput output =
        create_audio_output(options, &discovery, make_creator(&state));
    CHECK(output.sink != nullptr);
    CHECK(output.backend == "pulse");
    CHECK(state.pulse_calls == 1 && state.alsa_calls == 0);
  }
  {
    // auto 后端：PulseAudio 能看到 USB 声卡时选 pulse。
    CHECK(choose_audio_backend("auto", true, true,
                               "alsa_output.usb-Actions_USB_DISPLAY") == "pulse");
    // auto 后端：PulseAudio 看不到 USB 声卡时选 alsa。
    CHECK(choose_audio_backend("auto", true, false, "") == "alsa");
    CHECK(choose_audio_backend("auto", true, true, "") == "pulse");
    // 显式覆盖忽略环境。
    CHECK(choose_audio_backend("pulse", false, false, "") == "pulse");
    CHECK(choose_audio_backend("alsa", true, true,
                               "alsa_output.usb-Actions_USB_DISPLAY") == "alsa");
  }
  {
    // sink 名称匹配，覆盖大小写与 USB 标识。
    const std::string usb_name =
        "alsa_output.usb-Actions_USB_DISPLAY_51d3f01-02.analog-stereo";
    CHECK(select_usb_sink({make_sink("alsa_output.pci-0000_00_1f.3.analog-stereo"),
                           make_sink(usb_name)}) == usb_name);
    CHECK(select_usb_sink({make_sink("alsa_output.USBDisplay")}) ==
          "alsa_output.USBDisplay");
    CHECK(select_usb_sink({make_sink("usb-1DE1-f201")}) == "usb-1DE1-f201");
    CHECK(select_usb_sink({make_sink("alsa_output.pci-0000_00_1f.3")}).empty());
    CHECK(select_usb_sink({}).empty());
  }
  {
    // 自动模式优先使用 USB PulseAudio sink，并记录默认 sink。
    FakeDiscovery discovery;
    discovery.sinks = {
        make_sink("alsa_output.pci-0000_00_1f.3.analog-stereo"),
        make_sink("alsa_output.usb-Actions_USB_DISPLAY_51d3f01-02.analog-stereo")};
    CreatorState state;
    AudioOutput output = create_audio_output(make_options(true), &discovery,
                                             make_creator(&state));
    CHECK(output.sink != nullptr);
    CHECK(output.backend == "pulse");
    CHECK(output.device ==
          "alsa_output.usb-Actions_USB_DISPLAY_51d3f01-02.analog-stereo");
    CHECK(output.default_sink == discovery.default_name);
    CHECK(state.pulse_calls == 1 && state.alsa_calls == 0);
    CHECK(discovery.default_calls == 1);
    CHECK(discovery.default_name == "内置模拟输出");
  }
  {
    // 找不到 USB sink 时回退 ALSA。
    FakeDiscovery discovery;
    discovery.sinks = {make_sink("alsa_output.pci-0000_00_1f.3.analog-stereo")};
    CreatorState state;
    AudioOutput output = create_audio_output(make_options(true), &discovery,
                                             make_creator(&state));
    CHECK(output.sink != nullptr);
    CHECK(output.backend == "alsa");
    CHECK(output.device == "plughw:USBDISPLAY,0");
    CHECK(state.pulse_calls == 0 && state.alsa_calls == 1);
    CHECK(!output.error.empty());
  }
  {
    // PulseAudio 不可用时直接使用 ALSA。
    FakeDiscovery discovery;
    discovery.sinks = {
        make_sink("alsa_output.usb-Actions_USB_DISPLAY_51d3f01-02.analog-stereo")};
    CreatorState state;
    AudioOutput output = create_audio_output(make_options(false), &discovery,
                                             make_creator(&state));
    CHECK(output.sink != nullptr);
    CHECK(output.backend == "alsa");
    CHECK(state.pulse_calls == 0 && state.alsa_calls == 1);
  }
  {
    // PulseAudio 打开失败时回退 ALSA。
    FakeDiscovery discovery;
    discovery.sinks = {
        make_sink("alsa_output.usb-Actions_USB_DISPLAY_51d3f01-02.analog-stereo")};
    CreatorState state;
    state.fail_pulse = true;
    AudioOutput output = create_audio_output(make_options(true), &discovery,
                                             make_creator(&state));
    CHECK(output.sink != nullptr);
    CHECK(output.backend == "alsa");
    CHECK(state.pulse_calls == 1 && state.alsa_calls == 1);
    CHECK(!output.error.empty());
  }
  {
    // 指定后端为 ALSA 时不查询 PulseAudio sink 列表。
    FakeDiscovery discovery;
    CreatorState state;
    AudioOptions options = make_options(true);
    options.backend = "alsa";
    AudioOutput output =
        create_audio_output(options, &discovery, make_creator(&state));
    CHECK(output.sink != nullptr);
    CHECK(output.backend == "alsa");
    CHECK(state.pulse_calls == 0 && state.alsa_calls == 1);
    CHECK(discovery.list_calls == 0);
  }
  {
    // 显式指定 PulseAudio sink 时直接使用该名称。
    FakeDiscovery discovery;
    discovery.sinks = {make_sink("alsa_output.usb-other")};
    CreatorState state;
    AudioOptions options = make_options(true);
    options.backend = "pulse";
    options.pulse_sink = "alsa_output.usb-Actions_USB_DISPLAY_51d3f01-02.analog-stereo";
    AudioOutput output =
        create_audio_output(options, &discovery, make_creator(&state));
    CHECK(output.sink != nullptr);
    CHECK(output.backend == "pulse");
    CHECK(output.device == options.pulse_sink);
    CHECK(state.last_pulse_device == options.pulse_sink);
  }
  {
    // PulseAudio 查询失败时回退 ALSA。
    FakeDiscovery discovery;
    discovery.fail_list = true;
    CreatorState state;
    AudioOutput output = create_audio_output(make_options(true), &discovery,
                                             make_creator(&state));
    CHECK(output.sink != nullptr);
    CHECK(output.backend == "alsa");
    CHECK(output.default_sink == discovery.default_name);
    CHECK(!output.error.empty());
  }
  return testharness::finish("test_audio_backend");
}
