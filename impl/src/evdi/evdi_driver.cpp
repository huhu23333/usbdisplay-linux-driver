// SPDX-License-Identifier: MIT
#include "evdi/evdi_driver.h"

#include <poll.h>
#include <sys/select.h>
#include <unistd.h>

#include <cstdio>
#include <cstring>
#include <map>

#include "evdi_lib.h"

namespace usbdisplay {
namespace evdi {
namespace {

class LibevdiDriver : public EvdiDriver {
 public:
  ~LibevdiDriver() override { close(); }

  bool open(int card_index, std::string* err) override {
    close();
    handle_ = evdi_open(card_index);
    if (handle_ == EVDI_INVALID_HANDLE) {
      if (err) *err = "evdi_open card" + std::to_string(card_index) + " 失败";
      return false;
    }
    events_ = {};
    events_.mode_changed_handler = [](evdi_mode mode, void* data) {
      LibevdiDriver* self = static_cast<LibevdiDriver*>(data);
      if (!self->callbacks_.mode_changed) return;
      EvdiModeInfo info;
      info.width = mode.width;
      info.height = mode.height;
      info.refresh_rate = mode.refresh_rate;
      info.bits_per_pixel = mode.bits_per_pixel;
      info.pixel_format = mode.pixel_format;
      self->callbacks_.mode_changed(info);
    };
    events_.update_ready_handler = [](int id, void* data) {
      LibevdiDriver* self = static_cast<LibevdiDriver*>(data);
      if (self->callbacks_.update_ready) self->callbacks_.update_ready(id);
    };
    events_.dpms_handler = [](int mode, void* data) {
      LibevdiDriver* self = static_cast<LibevdiDriver*>(data);
      if (self->callbacks_.dpms_changed) self->callbacks_.dpms_changed(mode);
    };
    events_.crtc_state_handler = [](int state, void* data) {
      LibevdiDriver* self = static_cast<LibevdiDriver*>(data);
      if (self->callbacks_.crtc_state) self->callbacks_.crtc_state(state);
    };
    events_.cursor_set_handler = [](evdi_cursor_set cursor, void*) {
      std::free(cursor.buffer);
    };
    events_.cursor_move_handler = [](evdi_cursor_move, void*) {};
    events_.ddcci_data_handler = [](evdi_ddcci_data, void*) {};
    events_.user_data = this;
    evdi_enable_cursor_events(handle_, false);
    return true;
  }

  void close() override {
    if (handle_ != EVDI_INVALID_HANDLE) {
      evdi_close(handle_);
      handle_ = EVDI_INVALID_HANDLE;
    }
    buffers_.clear();
  }

  bool connect(const std::vector<uint8_t>& edid, int fps,
               std::string* err) override {
    if (handle_ == EVDI_INVALID_HANDLE) {
      if (err) *err = "evdi 未打开";
      return false;
    }
    const uint32_t pixel_area = 1920U * 1080U;
    if (fps != 60 && fps != 120) fps = 60;
    evdi_connect2(handle_, edid.data(), static_cast<unsigned int>(edid.size()),
                  pixel_area, pixel_area * static_cast<uint32_t>(fps));
    return true;
  }

  void disconnect() override {
    if (handle_ != EVDI_INVALID_HANDLE) evdi_disconnect(handle_);
  }

  void enable_cursor(bool enable) override {
    if (handle_ != EVDI_INVALID_HANDLE) evdi_enable_cursor_events(handle_, enable);
  }

  void set_callbacks(const EvdiEventCallbacks& callbacks) override {
    callbacks_ = callbacks;
  }

  bool register_buffer(const EvdiBufferDesc& buffer, std::string* err) override {
    if (handle_ == EVDI_INVALID_HANDLE) {
      if (err) *err = "evdi 未打开";
      return false;
    }
    evdi_buffer native{};
    native.id = buffer.id;
    native.buffer = buffer.data;
    native.width = buffer.width;
    native.height = buffer.height;
    native.stride = buffer.stride;
    native.rects = reinterpret_cast<evdi_rect*>(buffer.rects);
    native.rect_count = buffer.rect_count;
    evdi_register_buffer(handle_, native);
    buffers_[buffer.id] = buffer;
    return true;
  }

  void unregister_buffer(int buffer_id) override {
    if (handle_ != EVDI_INVALID_HANDLE) evdi_unregister_buffer(handle_, buffer_id);
    buffers_.erase(buffer_id);
  }

  bool request_update(int buffer_id, bool* immediate, std::string*) override {
    if (handle_ == EVDI_INVALID_HANDLE) return false;
    const bool ready = evdi_request_update(handle_, buffer_id);
    if (immediate != nullptr) *immediate = ready;
    return true;
  }

  int grab_pixels(int buffer_id, std::vector<EvdiRect>* rects,
                  std::string* err) override {
    auto it = buffers_.find(buffer_id);
    if (it == buffers_.end()) {
      if (err) *err = "evdi buffer 未注册";
      return -1;
    }
    EvdiBufferDesc& buffer = it->second;
    int count = buffer.rect_count;
    evdi_grab_pixels(handle_, reinterpret_cast<evdi_rect*>(buffer.rects), &count);
    if (rects != nullptr) {
      rects->clear();
      for (int i = 0; i < count; ++i) {
        EvdiRect rect;
        rect.x1 = buffer.rects[i].x1;
        rect.y1 = buffer.rects[i].y1;
        rect.x2 = buffer.rects[i].x2;
        rect.y2 = buffer.rects[i].y2;
        rects->push_back(rect);
      }
    }
    return count;
  }

  int event_fd() const override {
    return handle_ == EVDI_INVALID_HANDLE ? -1 : evdi_get_event_ready(handle_);
  }

  bool wait_events(int timeout_ms, bool* event_ready,
                   std::string* err) override {
    const int fd = event_fd();
    if (fd < 0) {
      if (err) *err = "evdi 事件 fd 无效";
      return false;
    }
    pollfd pfd{};
    pfd.fd = fd;
    pfd.events = POLLIN;
    const int result = ::poll(&pfd, 1, timeout_ms);
    if (result < 0) {
      if (errno == EINTR) return true;
      if (err) *err = std::string("evdi 事件 poll 失败：") + std::strerror(errno);
      return false;
    }
    if (result == 0) return true;
    if ((pfd.revents & (POLLERR | POLLHUP | POLLNVAL)) != 0) {
      std::fprintf(stderr,
                   "evdi wait_events: revents=0x%x errno=%d (%s)\n",
                   static_cast<unsigned>(pfd.revents), errno,
                   std::strerror(errno));
      if (err) *err = "evdi 事件 fd EOF/HUP";
      return false;
    }
    if ((pfd.revents & POLLIN) != 0) {
      evdi_handle_events(handle_, &events_);
      if (event_ready != nullptr) *event_ready = true;
    }
    return true;
  }

 private:
  evdi_handle handle_ = EVDI_INVALID_HANDLE;
  evdi_event_context events_{};
  EvdiEventCallbacks callbacks_;
  std::map<int, EvdiBufferDesc> buffers_;
};

}  // namespace

std::unique_ptr<EvdiDriver> make_libevdi_driver() {
  return std::unique_ptr<EvdiDriver>(new LibevdiDriver());
}

}  // namespace evdi
}  // namespace usbdisplay
