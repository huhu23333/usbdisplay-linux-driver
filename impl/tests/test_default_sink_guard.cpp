// SPDX-License-Identifier: MIT
#include <string>
#include <vector>

#include "audio/default_sink_guard.h"
#include "test_harness.h"

namespace {

class FakeDiscovery : public usbdisplay::audio::PulseDiscovery {
 public:
  std::string current = "内置模拟输出";
  bool fail_get = false;
  bool fail_set = false;
  int get_calls = 0;
  int set_calls = 0;
  std::string last_set;

  bool list_sinks(std::vector<usbdisplay::audio::PulseSinkInfo>*,
                  std::string*) override {
    return true;
  }
  bool get_default_sink(std::string* out, std::string* err) override {
    ++get_calls;
    if (fail_get) {
      if (err) *err = "查询失败";
      return false;
    }
    *out = current;
    return true;
  }
  bool set_default_sink(const std::string& name, std::string* err) override {
    ++set_calls;
    last_set = name;
    if (fail_set) {
      if (err) *err = "设置失败";
      return false;
    }
    current = name;
    return true;
  }
};

const char* kUsbSink =
    "alsa_output.usb-Actions_USB_DISPLAY_51d3f01-02.analog-stereo";

}  // namespace

int main() {
  using namespace usbdisplay::audio;
  {
    // 默认模式只读：开流前保存内置输出，开流时被切到 USB，不调用 set_default_sink。
    FakeDiscovery discovery;
    discovery.current = "内置模拟输出";
    std::string before_stream;
    std::string err;
    CHECK(discovery.get_default_sink(&before_stream, &err));
    discovery.current = kUsbSink;
    DefaultSinkGuard guard(&discovery, kUsbSink, false);
    CHECK(guard.capture_with_default(before_stream, &err));
    CHECK(guard.check_and_restore(&err));
    CHECK(discovery.set_calls == 0);
    CHECK(discovery.current == kUsbSink);
    CHECK(guard.exit_note().find("已记录") != std::string::npos);
    CHECK(guard.exit_note().find("未修改") != std::string::npos);
  }
  {
    // 显式开启恢复：使用开流前保存的值调用 set_default_sink。
    FakeDiscovery discovery;
    discovery.current = "内置模拟输出";
    std::string before_stream;
    std::string err;
    CHECK(discovery.get_default_sink(&before_stream, &err));
    discovery.current = kUsbSink;
    DefaultSinkGuard guard(&discovery, kUsbSink, true);
    CHECK(guard.capture_with_default(before_stream, &err));
    CHECK(guard.check_and_restore(&err));
    CHECK(discovery.set_calls == 1);
    CHECK(discovery.last_set == "内置模拟输出");
    CHECK(discovery.current == "内置模拟输出");
    CHECK(guard.restored());
    CHECK(guard.exit_note().find("已恢复") != std::string::npos);
  }
  {
    // 默认只读：默认 sink 未改变时不动作。
    FakeDiscovery discovery;
    DefaultSinkGuard guard(&discovery, kUsbSink, false);
    std::string err;
    CHECK(guard.capture(&err));
    CHECK(guard.check_and_restore(&err));
    CHECK(discovery.set_calls == 0);
    CHECK(guard.exit_note().find("未修改") != std::string::npos);
  }
  {
    // 默认只读：开流前默认已是 USB 输出，不动作。
    FakeDiscovery discovery;
    discovery.current = kUsbSink;
    DefaultSinkGuard guard(&discovery, kUsbSink, false);
    std::string err;
    CHECK(guard.capture(&err));
    CHECK(!guard.active());
    CHECK(guard.check_and_restore(&err));
    CHECK(discovery.set_calls == 0);
    CHECK(guard.exit_note().find("已是 USB") != std::string::npos);
  }
  {
    // 默认只读：当前默认不是 USB sink 时不动作。
    FakeDiscovery discovery;
    DefaultSinkGuard guard(&discovery, kUsbSink, false);
    std::string err;
    CHECK(guard.capture(&err));
    discovery.current = "hdmi 输出";
    CHECK(guard.check_and_restore(&err));
    CHECK(discovery.set_calls == 0);
  }
  {
    // 开启恢复但默认未改变时不动作。
    FakeDiscovery discovery;
    DefaultSinkGuard guard(&discovery, kUsbSink, true);
    std::string err;
    CHECK(guard.capture(&err));
    CHECK(guard.check_and_restore(&err));
    CHECK(discovery.set_calls == 0);
    CHECK(guard.exit_note() == "默认输出未修改");
  }
  {
    // 开启恢复，但开流前默认已是 USB 输出时不修改。
    FakeDiscovery discovery;
    discovery.current = kUsbSink;
    DefaultSinkGuard guard(&discovery, kUsbSink, true);
    std::string err;
    CHECK(guard.capture(&err));
    CHECK(guard.check_and_restore(&err));
    CHECK(discovery.set_calls == 0);
    CHECK(guard.exit_note().find("已是 USB") != std::string::npos);
  }
  {
    // 开启恢复时恢复失败的错误处理。
    FakeDiscovery discovery;
    DefaultSinkGuard guard(&discovery, kUsbSink, true);
    std::string err;
    CHECK(guard.capture(&err));
    discovery.current = kUsbSink;
    discovery.fail_set = true;
    CHECK(!guard.check_and_restore(&err));
    CHECK(discovery.set_calls == 1);
    CHECK(guard.failed());
    CHECK(guard.exit_note().find("恢复失败") != std::string::npos);
  }
  {
    // 默认只读模式下运行期间查询默认 sink 失败，不调用 set_default_sink。
    FakeDiscovery discovery;
    DefaultSinkGuard guard(&discovery, kUsbSink, false);
    std::string err;
    CHECK(guard.capture(&err));
    discovery.fail_get = true;
    CHECK(!guard.check_and_restore(&err));
    CHECK(discovery.set_calls == 0);
    CHECK(guard.failed());
    CHECK(guard.exit_note().find("查询失败") != std::string::npos);
  }
  {
    // 开流前读取失败时不启用保护，也不修改默认输出。
    FakeDiscovery discovery;
    discovery.fail_get = true;
    DefaultSinkGuard guard(&discovery, kUsbSink, true);
    std::string err;
    CHECK(!guard.capture(&err));
    CHECK(!guard.active());
    CHECK(!guard.check_and_restore(&err));
    CHECK(discovery.set_calls == 0);
    CHECK(guard.exit_note().find("未知") != std::string::npos);
  }
  return testharness::finish("test_default_sink_guard");
}
