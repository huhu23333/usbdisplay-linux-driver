// SPDX-License-Identifier: MIT
#include "audio/audio_pump.h"

#include <chrono>

#include "common/log.h"
#include "common/util.h"

namespace usbdisplay {
namespace audio {

AudioPump::AudioPump(AudioSink* sink, const AudioPumpConfig& config)
    : sink_(sink), config_(config) {}

AudioPump::~AudioPump() { stop(false); }

bool AudioPump::start(std::string* err) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (started_) return true;
  if (sink_ == nullptr) {
    if (err) *err = "音频播放接口为空";
    return false;
  }
  stop_requested_ = false;
  queue_.clear();
  queued_bytes_ = 0;
  started_ = true;
  thread_ = std::thread([this] { loop(); });
  return true;
}

bool AudioPump::queue_full() const {
  return queue_.size() >= config_.max_chunks ||
         queued_bytes_ >= config_.max_bytes;
}

bool AudioPump::push(const void* data, size_t bytes, uint64_t pts_us,
                     std::string* err) {
  if (data == nullptr || bytes == 0) {
    if (err) *err = "音频数据为空";
    return false;
  }
  std::unique_lock<std::mutex> lock(mutex_);
  if (!started_ || stop_requested_) {
    if (err) *err = "音频播放已停止";
    return false;
  }
  const auto deadline = std::chrono::steady_clock::now() +
                        std::chrono::milliseconds(config_.push_timeout_ms);
  while (queue_full()) {
    if (stop_requested_) {
      if (err) *err = "音频播放已停止";
      return false;
    }
    if (not_full_.wait_until(lock, deadline) == std::cv_status::timeout) {
      if (queue_full()) {
        if (err) *err = "音频队列已满";
        return false;
      }
    }
  }
  Chunk chunk;
  chunk.data.assign(static_cast<const uint8_t*>(data),
                    static_cast<const uint8_t*>(data) + bytes);
  chunk.pts_us = pts_us;
  queue_.push_back(std::move(chunk));
  queued_bytes_ += bytes;
  not_empty_.notify_one();
  return true;
}

void AudioPump::loop() {
  const auto interval =
      std::chrono::milliseconds(config_.periodic_check_ms > 0
                                    ? config_.periodic_check_ms
                                    : 1000);
  uint64_t last_check = mono_ms();
  for (;;) {
    Chunk chunk;
    {
      std::unique_lock<std::mutex> lock(mutex_);
      not_empty_.wait_for(lock, interval, [this] {
        return stop_requested_ || !queue_.empty();
      });
      if (queue_.empty()) {
        if (stop_requested_) break;
      } else {
        chunk = std::move(queue_.front());
        queue_.pop_front();
        queued_bytes_ -= chunk.data.size();
        not_full_.notify_all();
      }
    }
    if (config_.periodic_check) {
      const uint64_t now = mono_ms();
      if (now - last_check >= static_cast<uint64_t>(config_.periodic_check_ms)) {
        last_check = now;
        config_.periodic_check();
      }
    }
    if (chunk.data.empty()) continue;
    if (sink_ == nullptr || !sink_->is_open()) {
      failed_chunks_.fetch_add(1);
      continue;
    }
    std::string write_err;
    if (!sink_->write(chunk.data.data(), chunk.data.size(), &write_err)) {
      failed_chunks_.fetch_add(1);
      log::warn("音频写入失败：" + write_err);
      continue;
    }
    written_chunks_.fetch_add(1);
  }
}

void AudioPump::stop(bool drain, int drain_timeout_ms) {
  {
    std::unique_lock<std::mutex> lock(mutex_);
    if (!thread_.joinable()) {
      stop_requested_ = true;
      dropped_chunks_.fetch_add(queue_.size());
      queue_.clear();
      queued_bytes_ = 0;
      started_ = false;
      return;
    }
    stop_requested_ = true;
    not_empty_.notify_all();
    not_full_.notify_all();
    if (drain) {
      const auto deadline = std::chrono::steady_clock::now() +
                            std::chrono::milliseconds(drain_timeout_ms);
      not_empty_.notify_all();
      while (!queue_.empty()) {
        if (not_full_.wait_until(lock, deadline) == std::cv_status::timeout) {
          break;
        }
      }
    }
    if (!queue_.empty()) {
      dropped_chunks_.fetch_add(queue_.size());
      queue_.clear();
      queued_bytes_ = 0;
    }
    not_empty_.notify_all();
    not_full_.notify_all();
  }
  if (thread_.joinable()) thread_.join();
  std::lock_guard<std::mutex> lock(mutex_);
  started_ = false;
}

bool AudioPump::running() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return started_ && thread_.joinable();
}

uint64_t AudioPump::written_chunks() const { return written_chunks_.load(); }
uint64_t AudioPump::failed_chunks() const { return failed_chunks_.load(); }
uint64_t AudioPump::dropped_chunks() const { return dropped_chunks_.load(); }

}  // namespace audio
}  // namespace usbdisplay
