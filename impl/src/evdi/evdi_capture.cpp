// SPDX-License-Identifier: MIT
#include "evdi/evdi_capture.h"

#include <algorithm>
#include <cstdint>
#include <cstdio>

#include "common/log.h"
#include "common/util.h"

namespace usbdisplay {
namespace evdi {

struct EvdiCapture::Impl {
  std::unique_ptr<EvdiDriver> driver;
  EvdiEventCallbacks callbacks;
  FrameHandler handler;
  bool opened = false;
  bool connected = false;
  bool fatal = false;
  bool pending = false;
  bool immediate_pending = false;
  bool dpms_off = false;
  std::string error;
  int width = 0;
  int height = 0;
  int stride = 0;
  int buffer_id = 0;
  int next_buffer_id = 1;
  uint64_t generation = 0;
  uint64_t last_progress_ms = 0;
  uint64_t request_started_ms = 0;
  uint64_t connect_ms = 0;
  uint64_t last_force_grab_ms = 0;
  bool first_grab_done = false;
  bool initial_force_grab = false;
  int health_timeout_ms = 3000;
  std::vector<uint8_t> buffer;
  std::vector<EvdiRect> rects;

  void fail(const std::string& message) {
    fatal = true;
    error = message;
    pending = false;
    immediate_pending = false;
  }

  void cleanup_buffer() {
    if (driver && buffer_id != 0) driver->unregister_buffer(buffer_id);
    buffer_id = 0;
    buffer.clear();
    rects.clear();
    stride = 0;
  }

  void request_update() {
    if (!connected || buffer_id == 0 || fatal) return;
    bool immediate = false;
    std::string err;
    if (!driver->request_update(buffer_id, &immediate, &err)) {
      if (!err.empty()) {
        fail("request_update 失败：" + err);
      } else {
        pending = true;
        request_started_ms = mono_ms();
      }
      return;
    }
    if (immediate) {
      pending = false;
      immediate_pending = true;
    } else {
      pending = true;
      request_started_ms = mono_ms();
    }
  }

  void grab() {
    if (!connected || buffer_id == 0 || fatal) return;
    std::string err;
    const int count = driver->grab_pixels(buffer_id, &rects, &err);
    pending = false;
    if (count < 0) {
      fail("grab_pixels 失败：" + err);
      return;
    }
    if (count > 0) {
      if (handler && !buffer.empty()) {
        handler(buffer.data(), width, height, stride);
      }
      first_grab_done = true;
      last_progress_ms = mono_ms();
    }
    request_update();
  }

  void on_mode(const EvdiModeInfo& mode) {
    if (!connected) return;
    if (mode.width <= 0 || mode.height <= 0) {
      fail("evdi mode 尺寸无效");
      return;
    }
    ++generation;
    cleanup_buffer();
    width = mode.width;
    height = mode.height;
    stride = ((width + 63) & ~63) * 4;
    buffer.assign(static_cast<size_t>(stride) * height, 0);
    rects.assign(16, EvdiRect{});
    buffer_id = next_buffer_id++;
    EvdiBufferDesc desc;
    desc.id = buffer_id;
    desc.data = buffer.data();
    desc.width = width;
    desc.height = height;
    desc.stride = stride;
    desc.rects = rects.data();
    desc.rect_count = static_cast<int>(rects.size());
    std::string err;
    if (!driver->register_buffer(desc, &err)) {
      fail("register_buffer 失败：" + err);
      return;
    }
    pending = false;
    last_force_grab_ms = mono_ms();
    request_update();
    // 初始 mode 之后 Xorg 可能等 vblank，而 request_update(false) 不会
    // 产生 update_ready；首帧前主动 grab，避免 flip_done 死锁。
    if (initial_force_grab && !first_grab_done && !immediate_pending) grab();
  }

  void on_update(int id) {
    if (!connected || id != buffer_id || fatal) return;
    pending = false;
    grab();
  }

  void on_dpms(int mode) {
    if (!connected) return;
    dpms_off = mode == 0;
    if (dpms_off) {
      pending = false;
      immediate_pending = false;
    } else if (buffer_id != 0) {
      request_update();
    }
  }

  void check_health() {
    if (!connected || fatal) return;
    // request_update(false) 表示等待未来 dirty，静止桌面可能长时间没有
    // update_ready；这不是故障。只有连接后的首帧期限可安全判定失败。
    if (first_grab_done) return;
    const uint64_t now = mono_ms();
    if (now >= connect_ms &&
        now - connect_ms > static_cast<uint64_t>(health_timeout_ms)) {
      fail("evdi 连接后首帧超时");
    }
  }
};

EvdiCapture::EvdiCapture() : impl_(new Impl()) {
  impl_->driver = make_libevdi_driver();
}
EvdiCapture::EvdiCapture(std::unique_ptr<EvdiDriver> driver)
    : impl_(new Impl()) {
  impl_->driver = std::move(driver);
}
EvdiCapture::~EvdiCapture() { close(); }

bool EvdiCapture::open(int card_index, std::string* err) {
  close();
  if (!impl_->driver) {
    if (err) *err = "evdi 驱动为空";
    return false;
  }
  if (!impl_->driver->open(card_index, err)) return false;
  impl_->callbacks.mode_changed = [this](const EvdiModeInfo& mode) {
    impl_->on_mode(mode);
  };
  impl_->callbacks.update_ready = [this](int id) { impl_->on_update(id); };
  impl_->callbacks.dpms_changed = [this](int mode) { impl_->on_dpms(mode); };
  impl_->driver->set_callbacks(impl_->callbacks);
  impl_->opened = true;
  impl_->connected = false;
  impl_->fatal = false;
  impl_->pending = false;
  impl_->immediate_pending = false;
  impl_->error.clear();
  return true;
}

bool EvdiCapture::connect(const std::vector<uint8_t>& edid, std::string* err) {
  return connect(edid, 60, err);
}

bool EvdiCapture::connect(const std::vector<uint8_t>& edid, int fps,
                          std::string* err) {
  if (!impl_->opened || !impl_->driver) {
    if (err) *err = "evdi 未打开";
    return false;
  }
  if (!impl_->driver->connect(edid, fps, err)) return false;
  impl_->connected = true;
  impl_->fatal = false;
  impl_->pending = false;
  impl_->immediate_pending = false;
  impl_->first_grab_done = false;
  impl_->connect_ms = mono_ms();
  impl_->last_progress_ms = mono_ms();
  return true;
}

void EvdiCapture::disconnect() {
  if (!impl_->opened) return;
  impl_->cleanup_buffer();
  if (impl_->connected) impl_->driver->disconnect();
  impl_->connected = false;
  impl_->pending = false;
  impl_->immediate_pending = false;
  impl_->first_grab_done = false;
}

void EvdiCapture::close() {
  if (!impl_ || !impl_->opened) return;
  disconnect();
  impl_->driver->close();
  impl_->opened = false;
}

bool EvdiCapture::poll(int timeout_ms) {
  if (!impl_->opened || !impl_->driver || impl_->fatal) return false;
  bool event = false;
  std::string err;
  if (!impl_->driver->wait_events(timeout_ms, &event, &err)) {
    impl_->fail(err.empty() ? "evdi 事件循环失败" : err);
    return false;
  }
  for (int i = 0; i < 8 && impl_->immediate_pending && !impl_->fatal; ++i) {
    impl_->immediate_pending = false;
    impl_->grab();
  }
  // 首帧前定期主动 grab：初始 dirty 可能不触发 update_ready。
  if (impl_->initial_force_grab && !impl_->fatal && impl_->connected &&
      !impl_->first_grab_done && impl_->buffer_id != 0 &&
      impl_->last_force_grab_ms != 0 &&
      mono_ms() - impl_->last_force_grab_ms >= 100) {
    impl_->last_force_grab_ms = mono_ms();
    impl_->grab();
  }
  impl_->check_health();
  return !impl_->fatal;
}

void EvdiCapture::set_initial_force_grab(bool enable) {
  impl_->initial_force_grab = enable;
}

bool EvdiCapture::force_grab() {
  if (!impl_->opened || !impl_->connected || impl_->fatal) return false;
  impl_->grab();
  return !impl_->fatal;
}

bool EvdiCapture::connected() const { return impl_->connected; }
bool EvdiCapture::is_open() const { return impl_->opened; }
bool EvdiCapture::healthy(std::string* reason) const {
  if (impl_->fatal) {
    if (reason) *reason = impl_->error;
    return false;
  }
  return true;
}
bool EvdiCapture::stalled(uint64_t milliseconds) const {
  return impl_->last_progress_ms != 0 && mono_ms() > impl_->last_progress_ms &&
         mono_ms() - impl_->last_progress_ms > milliseconds;
}
uint64_t EvdiCapture::last_progress_ms() const { return impl_->last_progress_ms; }
void EvdiCapture::set_health_timeout_ms(int timeout_ms) {
  if (timeout_ms > 0) impl_->health_timeout_ms = timeout_ms;
}
void EvdiCapture::set_frame_handler(FrameHandler handler) {
  impl_->handler = std::move(handler);
}
int EvdiCapture::width() const { return impl_->width; }
int EvdiCapture::height() const { return impl_->height; }

}  // namespace evdi
}  // namespace usbdisplay
