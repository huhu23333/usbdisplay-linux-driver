// SPDX-License-Identifier: MIT
#include "audio/pulse_player.h"

#include "common/log.h"
#include "common/util.h"

#ifdef USBDISPLAY_HAVE_PULSE
#include <pulse/error.h>
#include <pulse/pulseaudio.h>
#endif

namespace usbdisplay {
namespace audio {
namespace {

#ifdef USBDISPLAY_HAVE_PULSE

bool iterate_mainloop(pa_mainloop* mainloop, int timeout_ms) {
  if (mainloop == nullptr) return false;
  if (pa_mainloop_prepare(mainloop, timeout_ms * 1000) < 0) return false;
  if (pa_mainloop_poll(mainloop) < 0) return false;
  if (pa_mainloop_dispatch(mainloop) < 0) return false;
  return true;
}

class LibpulseStreamDriver : public PulseStreamDriver {
 public:
  ~LibpulseStreamDriver() override { close(); }

  bool open(const PulseStreamOpenOptions& options, std::string* err) override {
    close();
    if (options.device.empty()) {
      if (err) *err = "PulseAudio sink 名称为空";
      return false;
    }
    if (options.sample_rate == 0 || options.channels == 0) {
      if (err) *err = "音频采样参数无效";
      return false;
    }
    mainloop_ = pa_mainloop_new();
    if (mainloop_ == nullptr) {
      if (err) *err = "创建 PulseAudio 主循环失败";
      return false;
    }
    pa_mainloop_api* api = pa_mainloop_get_api(mainloop_);
    context_ = pa_context_new(api, "usbdisplay");
    if (context_ == nullptr) {
      if (err) *err = "创建 PulseAudio 上下文失败";
      close();
      return false;
    }
    pa_context_set_state_callback(context_, context_state_callback, this);
    if (pa_context_connect(context_, nullptr, PA_CONTEXT_NOFLAGS, nullptr) < 0) {
      if (err) *err = "连接 PulseAudio 失败";
      close();
      return false;
    }
    const uint64_t context_deadline = mono_ms() + 3000;
    while (pa_context_get_state(context_) != PA_CONTEXT_READY) {
      if (!PA_CONTEXT_IS_GOOD(pa_context_get_state(context_))) {
        if (err) *err = error_.empty() ? "PulseAudio 上下文连接失败" : error_;
        close();
        return false;
      }
      if (mono_ms() >= context_deadline) {
        if (err) *err = "连接 PulseAudio 超时";
        close();
        return false;
      }
      if (!iterate_mainloop(mainloop_, 50)) {
        if (err) *err = "PulseAudio 主循环失败";
        close();
        return false;
      }
    }

    pa_sample_spec spec;
    spec.format = PA_SAMPLE_S16LE;
    spec.rate = options.sample_rate;
    spec.channels = options.channels;
    pa_buffer_attr buffer;
    buffer.maxlength = static_cast<uint32_t>(-1);
    buffer.tlength = static_cast<uint32_t>(
        static_cast<uint64_t>(spec.rate) * spec.channels * 2 *
        static_cast<uint64_t>(options.target_latency_ms) / 1000);
    buffer.prebuf = static_cast<uint32_t>(-1);
    buffer.minreq = static_cast<uint32_t>(-1);
    buffer.fragsize = static_cast<uint32_t>(-1);
    pa_stream_flags_t flags = PA_STREAM_DONT_MOVE;
    stream_ = pa_stream_new(context_, "usbdisplay", &spec, nullptr);
    if (stream_ == nullptr) {
      if (err) *err = "创建 PulseAudio 播放流失败";
      close();
      return false;
    }
    pa_stream_set_state_callback(stream_, stream_state_callback, this);
    pa_stream_set_write_callback(stream_, write_callback, this);
    pa_stream_set_moved_callback(stream_, moved_callback, this);
    if (pa_stream_connect_playback(stream_, options.device.c_str(), &buffer,
                                   flags, nullptr, nullptr) < 0) {
      if (err) *err = "连接 PulseAudio USB sink 失败";
      close();
      return false;
    }
    const uint64_t stream_deadline = mono_ms() + 3000;
    while (pa_stream_get_state(stream_) != PA_STREAM_READY) {
      if (!PA_STREAM_IS_GOOD(pa_stream_get_state(stream_))) {
        if (err) *err = error_.empty() ? "PulseAudio 播放流连接失败" : error_;
        close();
        return false;
      }
      if (mono_ms() >= stream_deadline) {
        if (err) *err = "连接 PulseAudio 播放流超时";
        close();
        return false;
      }
      if (!iterate_mainloop(mainloop_, 50)) {
        if (err) *err = "PulseAudio 主循环失败";
        close();
        return false;
      }
    }
    device_ = options.device;
    sample_rate_ = options.sample_rate;
    channels_ = options.channels;
    log::info("pulse: 输出到 " + device_ + " " +
              std::to_string(sample_rate_) + "Hz/" +
              std::to_string(channels_) + " 声道，DONT_MOVE");
    return true;
  }

  bool write(const void* data, size_t bytes, std::string* err) override {
    if (stream_ == nullptr || !is_open()) {
      if (err) *err = "PulseAudio 输出未打开";
      return false;
    }
    if (data == nullptr || bytes == 0) {
      if (err) *err = "音频数据为空";
      return false;
    }
    const uint64_t deadline = mono_ms() + 500;
    while (mono_ms() < deadline) {
      if (failed_) {
        if (err) *err = error_.empty() ? "PulseAudio 播放流失败" : error_;
        return false;
      }
      const size_t writable = pa_stream_writable_size(stream_);
      if (writable >= bytes) {
        if (pa_stream_write(stream_, data, bytes, nullptr, 0,
                            PA_SEEK_RELATIVE) < 0) {
          if (err) *err = "PulseAudio 写入失败";
          return false;
        }
        iterate_mainloop(mainloop_, 0);
        return true;
      }
      if (!iterate_mainloop(mainloop_, 50)) {
        if (err) *err = "PulseAudio 主循环失败";
        return false;
      }
    }
    if (err) *err = "PulseAudio 写入超时";
    return false;
  }

  void close() override {
    if (stream_ != nullptr) {
      pa_stream_disconnect(stream_);
      pa_stream_unref(stream_);
      stream_ = nullptr;
    }
    if (context_ != nullptr) {
      pa_context_disconnect(context_);
      pa_context_unref(context_);
      context_ = nullptr;
    }
    if (mainloop_ != nullptr) {
      pa_mainloop_free(mainloop_);
      mainloop_ = nullptr;
    }
    device_.clear();
    failed_ = false;
    error_.clear();
  }

  bool is_open() const override {
    return stream_ != nullptr && pa_stream_get_state(stream_) == PA_STREAM_READY;
  }

  std::string describe() const override {
    if (!is_open()) return "pulse:closed";
    return "pulse:" + device_;
  }

 private:
  static void context_state_callback(pa_context* context, void* data) {
    LibpulseStreamDriver* self = static_cast<LibpulseStreamDriver*>(data);
    const pa_context_state_t state = pa_context_get_state(context);
    if (state == PA_CONTEXT_FAILED || state == PA_CONTEXT_TERMINATED) {
      self->failed_ = true;
      if (self->error_.empty()) self->error_ = "PulseAudio 上下文连接断开";
    }
  }

  static void stream_state_callback(pa_stream* stream, void* data) {
    LibpulseStreamDriver* self = static_cast<LibpulseStreamDriver*>(data);
    const pa_stream_state_t state = pa_stream_get_state(stream);
    if (state == PA_STREAM_FAILED || state == PA_STREAM_TERMINATED) {
      self->failed_ = true;
      if (self->error_.empty()) self->error_ = "PulseAudio 播放流断开";
    }
  }

  static void write_callback(pa_stream*, size_t, void*) {
    // 写入由 AudioPump 线程驱动，这里只响应可写事件。
  }

  static void moved_callback(pa_stream*, void*) {
    // 刻意忽略 PA_STREAM_MOVED 类事件，不移动流，也不修改默认 sink。
  }

  pa_mainloop* mainloop_ = nullptr;
  pa_context* context_ = nullptr;
  pa_stream* stream_ = nullptr;
  std::string device_;
  uint32_t sample_rate_ = 0;
  uint16_t channels_ = 0;
  bool failed_ = false;
  std::string error_;
};

#else  // !USBDISPLAY_HAVE_PULSE

class LibpulseStreamDriver : public PulseStreamDriver {
 public:
  bool open(const PulseStreamOpenOptions&, std::string* err) override {
    if (err) *err = "构建时未启用 PulseAudio 支持";
    return false;
  }
  bool write(const void*, size_t, std::string* err) override {
    if (err) *err = "构建时未启用 PulseAudio 支持";
    return false;
  }
  void close() override {}
  bool is_open() const override { return false; }
  std::string describe() const override { return "pulse:unavailable"; }
};

#endif

}  // namespace

std::unique_ptr<PulseStreamDriver> make_libpulse_stream_driver() {
  return std::unique_ptr<PulseStreamDriver>(new LibpulseStreamDriver());
}

PulsePlayer::PulsePlayer() = default;
PulsePlayer::PulsePlayer(std::unique_ptr<PulseStreamDriver> driver)
    : driver_(std::move(driver)) {}
PulsePlayer::~PulsePlayer() { close(); }

bool PulsePlayer::open(const std::string& device, uint32_t sample_rate,
                       uint16_t channels, std::string* err) {
  if (!driver_) driver_ = make_libpulse_stream_driver();
  PulseStreamOpenOptions options;
  options.device = device;
  options.sample_rate = sample_rate;
  options.channels = channels;
  options.target_latency_ms = 100;
  options.dont_move = true;
  return driver_->open(options, err);
}

bool PulsePlayer::write(const void* data, size_t bytes, std::string* err) {
  if (!driver_) {
    if (err) *err = "PulseAudio 输出未打开";
    return false;
  }
  return driver_->write(data, bytes, err);
}

void PulsePlayer::close() {
  if (driver_) driver_->close();
}

bool PulsePlayer::is_open() const {
  return driver_ && driver_->is_open();
}

std::string PulsePlayer::describe() const {
  if (!driver_) return "pulse:closed";
  return driver_->describe();
}

}  // namespace audio
}  // namespace usbdisplay
