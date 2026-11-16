// SPDX-License-Identifier: MIT
// 音频播放线程与有界队列。发送线程只负责入队，ALSA 写入在独立线程完成。
#pragma once

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "audio/audio_sink.h"

namespace usbdisplay {
namespace audio {

struct AudioPumpConfig {
  size_t max_chunks = 8;
  size_t max_bytes = 1024 * 1024;
  int push_timeout_ms = 200;
  // 播放期间的周期性检查，例如默认输出保护。
  std::function<void()> periodic_check;
  int periodic_check_ms = 1000;
};

class AudioPump {
 public:
  AudioPump(AudioSink* sink, const AudioPumpConfig& config = AudioPumpConfig());
  ~AudioPump();
  AudioPump(const AudioPump&) = delete;
  AudioPump& operator=(const AudioPump&) = delete;

  bool start(std::string* err);
  // 停止入队并等待播放线程退出。drain 为真时先尝试排空队列，
  // 超过超时时间后丢弃剩余数据。
  void stop(bool drain, int drain_timeout_ms = 500);
  bool push(const void* data, size_t bytes, uint64_t pts_us,
            std::string* err);

  bool running() const;
  uint64_t written_chunks() const;
  uint64_t failed_chunks() const;
  uint64_t dropped_chunks() const;

 private:
  struct Chunk {
    std::vector<uint8_t> data;
    uint64_t pts_us = 0;
  };

  void loop();
  bool queue_full() const;

  AudioSink* sink_;
  AudioPumpConfig config_;
  mutable std::mutex mutex_;
  std::condition_variable not_empty_;
  std::condition_variable not_full_;
  std::deque<Chunk> queue_;
  size_t queued_bytes_ = 0;
  bool stop_requested_ = false;
  bool started_ = false;
  std::thread thread_;
  std::atomic<uint64_t> written_chunks_{0};
  std::atomic<uint64_t> failed_chunks_{0};
  std::atomic<uint64_t> dropped_chunks_{0};
};

}  // namespace audio
}  // namespace usbdisplay
