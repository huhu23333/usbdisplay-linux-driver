// SPDX-License-Identifier: MIT
// 三槽共享帧：writer 发布完整帧，reader 只读映射并通过 Unix socket 租约取帧。
#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace usbdisplay {
namespace evdi {

constexpr uint64_t kSharedFrameMagic = 0x5553424450524f54ULL;
constexpr uint32_t kSharedFrameVersion = 2;
constexpr uint32_t kSharedFrameHeaderSize = 4096;
constexpr uint32_t kSharedFrameSlotCount = 3;
constexpr uint32_t kSharedFrameFormatXrgb8888 = 1;
constexpr const char* kDefaultSharedFramePath = "/dev/shm/usbdisplay-frames";
constexpr const char* kDefaultLeaseSocketPath = "/run/usbdisplay/frames.sock";

enum class SlotState : uint32_t {
  Free = 0,
  Writing = 1,
  Ready = 2,
  Reading = 3,
};

struct SharedFrameLayout {
  uint32_t width = 1920;
  uint32_t height = 1080;
  uint32_t stride = 7680;
  uint32_t format = kSharedFrameFormatXrgb8888;
  size_t frame_bytes() const;
  size_t slot_stride() const;
  size_t total_size() const;
  size_t slot_offset(uint32_t slot) const;
};

bool validate_shared_layout(const SharedFrameLayout& layout, std::string* err);

struct SharedFrameHeader {
  uint64_t magic = kSharedFrameMagic;
  uint32_t version = kSharedFrameVersion;
  uint32_t header_size = kSharedFrameHeaderSize;
  uint32_t width = 1920;
  uint32_t height = 1080;
  uint32_t stride = 7680;
  uint32_t format = kSharedFrameFormatXrgb8888;
  uint32_t slot_count = kSharedFrameSlotCount;
  uint32_t reserved0 = 0;
  uint64_t slot_stride = 0;
  uint64_t total_size = 0;
  uint64_t generation = 0;
  std::atomic<uint32_t> ready{0};
  std::atomic<uint64_t> heartbeat_ns{0};
  std::atomic<uint32_t> writer_pid{0};
  std::atomic<uint64_t> latest_token{0};
  std::atomic<uint32_t> slot_state[kSharedFrameSlotCount]{};
  std::atomic<uint64_t> slot_seq[kSharedFrameSlotCount]{};
  std::atomic<uint64_t> slot_timestamp_ns[kSharedFrameSlotCount]{};
  std::atomic<uint32_t> slot_generation[kSharedFrameSlotCount]{};
  std::atomic<uint64_t> dropped_frames{0};
};
static_assert(sizeof(SharedFrameHeader) <= kSharedFrameHeaderSize,
              "共享帧头超过 4096 字节");
static_assert(std::atomic<uint32_t>::is_always_lock_free,
              "共享帧需要 lock-free 32 位原子");
static_assert(std::atomic<uint64_t>::is_always_lock_free,
              "共享帧需要 lock-free 64 位原子");

struct SharedLeaseInfo {
  uint64_t token = 0;
  uint32_t slot = 0;
  uint32_t width = 0;
  uint32_t height = 0;
  uint32_t stride = 0;
  uint64_t seq = 0;
  uint64_t generation = 0;
  uint64_t offset = 0;
  uint64_t payload = 0;
};

struct SharedFrameView {
  std::vector<uint8_t> data;
  uint32_t width = 0;
  uint32_t height = 0;
  uint32_t stride = 0;
  uint32_t format = 0;
  uint64_t seq = 0;
  uint64_t timestamp_ns = 0;
  uint64_t generation = 0;
};

class SharedFrameWriter {
 public:
  explicit SharedFrameWriter(const SharedFrameLayout& layout = SharedFrameLayout());
  ~SharedFrameWriter();
  SharedFrameWriter(const SharedFrameWriter&) = delete;
  SharedFrameWriter& operator=(const SharedFrameWriter&) = delete;

  bool open(const std::string& path, std::string* err);
  bool publish(const uint8_t* frame, int source_stride, std::string* err);
  void heartbeat();
  void invalidate();
  void close();
  bool is_open() const;
  uint64_t dropped_frames() const;

 private:
  friend class FrameLeaseServer;
  bool lease_acquire(SharedLeaseInfo* info);
  bool lease_release(uint64_t token);
  SharedFrameHeader* header();

  SharedFrameLayout layout_;
  std::string path_;
  std::string temp_path_;
  uint64_t dev_ = 0;
  uint64_t ino_ = 0;
  int fd_ = -1;
  void* mapping_ = nullptr;
  SharedFrameHeader* header_ = nullptr;
  uint64_t next_seq_ = 1;
  bool leased_[kSharedFrameSlotCount] = {false, false, false};
};

enum class FrameReadResult { Ok, NoFrame, Error };

class FrameLeaseClient;

class SharedFrameReader {
 public:
  SharedFrameReader();
  ~SharedFrameReader();
  SharedFrameReader(const SharedFrameReader&) = delete;
  SharedFrameReader& operator=(const SharedFrameReader&) = delete;

  bool open(const std::string& path, const std::string& socket_path,
            std::string* err);
  FrameReadResult read(SharedFrameView* frame, std::string* err);
  bool file_replaced(const std::string& path) const;
  bool socket_connected() const;
  uint64_t generation() const;
  uint64_t heartbeat_age_ms() const;
  void close();
  bool is_open() const;

 private:
  std::string path_;
  int fd_ = -1;
  void* mapping_ = nullptr;
  SharedFrameHeader* header_ = nullptr;
  size_t mapping_size_ = 0;
  std::unique_ptr<FrameLeaseClient> client_;
};

}  // namespace evdi
}  // namespace usbdisplay
