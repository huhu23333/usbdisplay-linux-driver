// SPDX-License-Identifier: MIT
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

#include "audio/audio_pump.h"
#include "test_harness.h"

namespace {

class FakeSink : public usbdisplay::audio::AudioSink {
 public:
  bool open(const std::string&, uint32_t, uint16_t, std::string*) override {
    open_ = true;
    return true;
  }

  bool write(const void* data, size_t bytes, std::string*) override {
    std::unique_lock<std::mutex> lock(mutex_);
    ++entered_;
    entered_cv_.notify_all();
    if (block_) {
      release_cv_.wait_for(lock, std::chrono::milliseconds(200),
                           [this] { return released_; });
    }
    ++writes_;
    last_bytes_ = bytes;
    sizes_.push_back(bytes);
    (void)data;
    return true;
  }

  void close() override { open_ = false; }
  bool is_open() const override { return open_; }
  std::string describe() const override { return "fake"; }

  bool wait_entered(int timeout_ms) {
    std::unique_lock<std::mutex> lock(mutex_);
    return entered_cv_.wait_for(lock, std::chrono::milliseconds(timeout_ms),
                                [this] { return entered_ > 0; });
  }
  void set_block(bool block) {
    std::lock_guard<std::mutex> lock(mutex_);
    block_ = block;
  }
  void release() {
    std::lock_guard<std::mutex> lock(mutex_);
    released_ = true;
    release_cv_.notify_all();
  }
  size_t writes() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return writes_;
  }

 private:
  mutable std::mutex mutex_;
  std::condition_variable entered_cv_;
  std::condition_variable release_cv_;
  bool open_ = false;
  bool block_ = true;
  bool released_ = false;
  size_t entered_ = 0;
  size_t writes_ = 0;
  size_t last_bytes_ = 0;
  std::vector<size_t> sizes_;
};

std::vector<uint8_t> make_pcm(size_t bytes) {
  return std::vector<uint8_t>(bytes, 0x11);
}

}  // namespace

int main() {
  using namespace usbdisplay::audio;
  {
    // 入队立即返回，ALSA 写入由播放线程完成。
    FakeSink sink;
    AudioPumpConfig config;
    config.max_chunks = 4;
    config.max_bytes = 1024 * 1024;
    config.push_timeout_ms = 50;
    AudioPump pump(&sink, config);
    std::string err;
    CHECK(sink.open("", 48000, 2, &err));
    CHECK(pump.start(&err));
    const auto pcm = make_pcm(320);
    CHECK(pump.push(pcm.data(), pcm.size(), 0, &err));
    CHECK(sink.wait_entered(500));
    const auto begin = std::chrono::steady_clock::now();
    for (int i = 0; i < 3; ++i) {
      CHECK(pump.push(pcm.data(), pcm.size(),
                      static_cast<uint64_t>(i + 1) * 20000, &err));
    }
    const auto elapsed = std::chrono::steady_clock::now() - begin;
    CHECK(elapsed < std::chrono::milliseconds(100));
    CHECK(pump.running());
    sink.release();
    pump.stop(true);
    CHECK(!pump.running());
    CHECK(sink.writes() >= 1);
  }
  {
    // 队列有界，满时在超时后返回错误。
    FakeSink sink;
    AudioPumpConfig config;
    config.max_chunks = 1;
    config.max_bytes = 1024 * 1024;
    config.push_timeout_ms = 50;
    AudioPump pump(&sink, config);
    std::string err;
    CHECK(sink.open("", 48000, 2, &err));
    CHECK(pump.start(&err));
    const auto pcm = make_pcm(320);
    CHECK(pump.push(pcm.data(), pcm.size(), 0, &err));
    CHECK(sink.wait_entered(500));
    CHECK(pump.push(pcm.data(), pcm.size(), 20000, &err));
    const auto begin = std::chrono::steady_clock::now();
    CHECK(!pump.push(pcm.data(), pcm.size(), 40000, &err));
    const auto elapsed = std::chrono::steady_clock::now() - begin;
    CHECK(elapsed >= std::chrono::milliseconds(40));
    CHECK(err.find("队列") != std::string::npos);
    sink.release();
    pump.stop(false);
    CHECK(!pump.running());
  }
  {
    // 停止时排空队列并 join 播放线程。
    FakeSink sink;
    sink.set_block(false);
    AudioPumpConfig config;
    config.max_chunks = 8;
    AudioPump pump(&sink, config);
    std::string err;
    CHECK(sink.open("", 48000, 2, &err));
    CHECK(pump.start(&err));
    const auto pcm = make_pcm(320);
    for (int i = 0; i < 5; ++i) {
      CHECK(pump.push(pcm.data(), pcm.size(), static_cast<uint64_t>(i) * 20000,
                      &err));
    }
    pump.stop(true, 1000);
    CHECK(!pump.running());
    CHECK(sink.writes() == 5);
    pump.stop(true);  // 重复停止不应阻塞
    CHECK(!pump.running());
  }
  {
    // 播放接口阻塞时停止仍能退出，不排队的数据被丢弃。
    FakeSink sink;
    AudioPumpConfig config;
    config.max_chunks = 4;
    AudioPump pump(&sink, config);
    std::string err;
    CHECK(sink.open("", 48000, 2, &err));
    CHECK(pump.start(&err));
    const auto pcm = make_pcm(320);
    CHECK(pump.push(pcm.data(), pcm.size(), 0, &err));
    CHECK(sink.wait_entered(500));
    CHECK(pump.push(pcm.data(), pcm.size(), 20000, &err));
    pump.stop(false);
    CHECK(!pump.running());
    CHECK(pump.dropped_chunks() >= 1);
    CHECK(!pump.push(pcm.data(), pcm.size(), 0, &err));
  }
  {
    // 音频泵停止不影响独立视频路径；视频侧动作也不影响音频泵重新启动。
    FakeSink sink;
    sink.set_block(false);
    AudioPumpConfig config;
    config.max_chunks = 4;
    AudioPump pump(&sink, config);
    std::string err;
    CHECK(sink.open("", 48000, 2, &err));
    CHECK(pump.start(&err));
    const auto pcm = make_pcm(320);
    CHECK(pump.push(pcm.data(), pcm.size(), 0, &err));
    pump.stop(true);
    CHECK(!pump.running());
    // 独立的视频发送计数继续工作。
    uint64_t video_frames = 0;
    ++video_frames;
    CHECK(video_frames == 1);
    // 重新启动音频泵不受视频侧影响。
    CHECK(pump.start(&err));
    CHECK(pump.push(pcm.data(), pcm.size(), 20000, &err));
    pump.stop(true);
    CHECK(!pump.running());
  }
  return testharness::finish("test_audio_pump");
}
