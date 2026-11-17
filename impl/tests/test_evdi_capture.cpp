// SPDX-License-Identifier: MIT
#include <unistd.h>

#include <cstdint>
#include <string>
#include <vector>

#include "evdi/evdi_capture.h"
#include "test_harness.h"

namespace {

class FakeDriver : public usbdisplay::evdi::EvdiDriver {
 public:
  usbdisplay::evdi::EvdiEventCallbacks callbacks;
  bool opened = false;
  bool connected = false;
  int immediate_remaining = 0;
  int grab_rects = 1;
  bool grab_error = false;
  bool request_error = false;
  bool wait_error = false;
  int last_buffer_id = 0;
  int registered = 0;

  bool open(int, std::string*) override {
    opened = true;
    return true;
  }
  void close() override { opened = false; }
  bool connect(const std::vector<uint8_t>&, int, std::string*) override {
    connected = true;
    return true;
  }
  void disconnect() override { connected = false; }
  void enable_cursor(bool) override {}
  void set_callbacks(const usbdisplay::evdi::EvdiEventCallbacks& value) override {
    callbacks = value;
  }
  bool register_buffer(const usbdisplay::evdi::EvdiBufferDesc& buffer,
                       std::string*) override {
    last_buffer_id = buffer.id;
    ++registered;
    return true;
  }
  void unregister_buffer(int) override {}
  bool request_update(int, bool* immediate, std::string* err) override {
    if (request_error) {
      if (err) *err = "ioctl 失败";
      return false;
    }
    if (immediate != nullptr) *immediate = immediate_remaining > 0;
    if (immediate_remaining > 0) --immediate_remaining;
    return true;
  }
  int grab_pixels(int, std::vector<usbdisplay::evdi::EvdiRect>* rects,
                  std::string* err) override {
    if (grab_error) {
      if (err) *err = "grab 失败";
      return -1;
    }
    if (rects != nullptr) rects->assign(grab_rects, usbdisplay::evdi::EvdiRect{});
    return grab_rects;
  }
  int event_fd() const override { return -1; }
  bool wait_events(int, bool* event_ready, std::string* err) override {
    if (wait_error) {
      if (err) *err = "EOF";
      return false;
    }
    if (event_ready != nullptr) *event_ready = false;
    return true;
  }

  void emit_mode(int width, int height) {
    usbdisplay::evdi::EvdiModeInfo mode;
    mode.width = width;
    mode.height = height;
    mode.bits_per_pixel = 32;
    if (callbacks.mode_changed) callbacks.mode_changed(mode);
  }
  void emit_update(int id) {
    if (callbacks.update_ready) callbacks.update_ready(id);
  }
  void emit_dpms(int mode) {
    if (callbacks.dpms_changed) callbacks.dpms_changed(mode);
  }
};

class Harness {
 public:
  FakeDriver* fake;
  usbdisplay::evdi::EvdiCapture capture;
  std::string err;
  int frames = 0;

  Harness()
      : fake(new FakeDriver()),
        capture(std::unique_ptr<usbdisplay::evdi::EvdiDriver>(fake)) {
    capture.set_health_timeout_ms(30);
    capture.set_frame_handler([this](const uint8_t*, int, int, int) { ++frames; });
    CHECK(capture.open(3, &err));
    CHECK(capture.connect(std::vector<uint8_t>{1, 2, 3}, &err));
  }
};

}  // namespace

int main() {
  using namespace usbdisplay::evdi;
  {
    Harness h;
    h.fake->immediate_remaining = 0;
    h.fake->emit_mode(1920, 1080);
    usleep(50000);
    CHECK(!h.capture.poll(0));
    std::string reason;
    CHECK(!h.capture.healthy(&reason));
    CHECK(reason.find("超时") != std::string::npos);
  }
  {
    // 首次成功抓取后，异步请求长期无事件。
    Harness h;
    h.fake->immediate_remaining = 1;
    h.fake->emit_mode(1920, 1080);
    CHECK(h.capture.poll(0));
    CHECK(h.frames == 1);
    usleep(50000);
    // 首次成功后 request_update(false) 是正常异步等待；静止桌面没有新
    // update_ready 不能判为故障，只能由首帧期限处理。
    CHECK(h.capture.poll(0));
    std::string reason;
    CHECK(h.capture.healthy(&reason));
  }
  {
    // 连续零 grab 后停滞。
    Harness h;
    h.fake->immediate_remaining = 1;
    h.fake->grab_rects = 0;
    h.fake->emit_mode(1920, 1080);
    CHECK(h.capture.poll(0));
    usleep(50000);
    CHECK(!h.capture.poll(0));
    CHECK(h.frames == 0);
  }
  {
    // EOF/HUP。
    Harness h;
    h.fake->wait_error = true;
    CHECK(!h.capture.poll(0));
    std::string reason;
    CHECK(!h.capture.healthy(&reason));
    CHECK(reason.find("EOF") != std::string::npos);
  }
  {
    // mode/DPMS 交错时旧 buffer id 事件被忽略，新模式可继续抓取。
    Harness h;
    h.fake->emit_mode(1920, 1080);
    const int first_id = h.fake->last_buffer_id;
    h.fake->emit_mode(1920, 1080);
    const int second_id = h.fake->last_buffer_id;
    CHECK(first_id != second_id);
    h.fake->emit_update(first_id);
    CHECK(h.frames == 0);
    h.fake->emit_dpms(0);
    h.fake->emit_dpms(1);
    h.fake->emit_update(second_id);
    CHECK(h.frames == 1);
    h.fake->emit_update(second_id);
    CHECK(h.frames == 2);
  }
  {
    // grab 失败立即进入不健康状态。
    Harness h;
    h.fake->grab_error = true;
    h.fake->emit_mode(1920, 1080);
    h.fake->emit_update(h.fake->last_buffer_id);
    std::string reason;
    CHECK(!h.capture.healthy(&reason));
    CHECK(reason.find("grab") != std::string::npos);
  }
  {
    // request ioctl 错误立即进入不健康状态。
    Harness h;
    h.fake->request_error = true;
    h.fake->emit_mode(1920, 1080);
    std::string reason;
    CHECK(!h.capture.healthy(&reason));
    CHECK(reason.find("request") != std::string::npos);
  }
  return testharness::finish("test_evdi_capture");
}
