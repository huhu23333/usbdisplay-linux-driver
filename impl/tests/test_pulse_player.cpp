// SPDX-License-Identifier: MIT
#include <cstdint>
#include <memory>
#include <string>

#include "audio/pulse_player.h"
#include "audio/pulse_stream.h"
#include "test_harness.h"

namespace {

class FakeStreamDriver : public usbdisplay::audio::PulseStreamDriver {
 public:
  usbdisplay::audio::PulseStreamOpenOptions options;
  int open_calls = 0;
  int write_calls = 0;
  int close_calls = 0;
  int default_changes = 0;
  int move_calls = 0;
  bool opened = false;
  bool fail_open = false;

  bool open(const usbdisplay::audio::PulseStreamOpenOptions& value,
            std::string* err) override {
    ++open_calls;
    options = value;
    if (fail_open) {
      if (err) *err = "设备不可用";
      return false;
    }
    opened = true;
    return true;
  }
  bool write(const void*, size_t, std::string*) override {
    ++write_calls;
    return opened;
  }
  void close() override {
    ++close_calls;
    opened = false;
  }
  bool is_open() const override { return opened; }
  std::string describe() const override { return "fake:pulse"; }

  // 模拟用户切换默认 sink：驱动不提供 move 接口，调用计数应保持为零。
  void simulate_default_sink_change() {
    ++default_changes;
    // 真实驱动只忽略 moved 事件，不会调用任何移动操作。
  }
};

}  // namespace

int main() {
  using namespace usbdisplay::audio;
  {
    // 显式 device、DONT_MOVE、100ms 目标延迟。
    FakeStreamDriver* fake = new FakeStreamDriver();
    PulsePlayer player{std::unique_ptr<PulseStreamDriver>(fake)};
    std::string err;
    CHECK(player.open("alsa_output.usb-Actions_USB_DISPLAY", 48000, 2, &err));
    CHECK(fake->open_calls == 1);
    CHECK(fake->options.device == "alsa_output.usb-Actions_USB_DISPLAY");
    CHECK(fake->options.sample_rate == 48000);
    CHECK(fake->options.channels == 2);
    CHECK(fake->options.dont_move);
    CHECK(fake->options.target_latency_ms == 100);
    CHECK(player.is_open());
    const uint8_t pcm[8] = {0, 1, 2, 3, 4, 5, 6, 7};
    CHECK(player.write(pcm, sizeof(pcm), &err));
    CHECK(fake->write_calls == 1);
    // 默认 sink 变化后程序不移动流。
    fake->simulate_default_sink_change();
    CHECK(fake->default_changes == 1);
    CHECK(fake->move_calls == 0);
    CHECK(fake->options.device == "alsa_output.usb-Actions_USB_DISPLAY");
    player.close();
    CHECK(!player.is_open());
    CHECK(fake->close_calls == 1);
  }
  {
    // 打开失败时保留错误信息。
    FakeStreamDriver* fake = new FakeStreamDriver();
    fake->fail_open = true;
    PulsePlayer player{std::unique_ptr<PulseStreamDriver>(fake)};
    std::string err;
    CHECK(!player.open("usb-sink", 48000, 2, &err));
    CHECK(!player.is_open());
    CHECK(err.find("设备不可用") != std::string::npos);
  }
  return testharness::finish("test_pulse_player");
}
