// SPDX-License-Identifier: MIT
#include "evdi/shared_frame.h"

#include <fcntl.h>
#include <signal.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include <cerrno>
#include <cstddef>
#include <cstring>
#include <new>

#include "evdi/frame_lease_client.h"

namespace usbdisplay {
namespace evdi {
namespace {

uint64_t monotonic_ns() {
  timespec value{};
  clock_gettime(CLOCK_MONOTONIC, &value);
  return static_cast<uint64_t>(value.tv_sec) * 1000000000ULL +
         static_cast<uint64_t>(value.tv_nsec);
}
uint32_t current_pid() { return static_cast<uint32_t>(::getpid()); }
bool pid_alive(uint32_t pid) {
  return pid != 0 && (::kill(static_cast<pid_t>(pid), 0) == 0 || errno == EPERM);
}
bool read_field(int fd, off_t offset, void* data, size_t size) {
  return ::pread(fd, data, size, offset) == static_cast<ssize_t>(size);
}
bool target_writer_active(const std::string& path, std::string* err) {
  struct stat info {};
  if (::lstat(path.c_str(), &info) != 0) {
    if (errno == ENOENT) return false;
    if (err) *err = std::strerror(errno);
    return true;
  }
  if (!S_ISREG(info.st_mode) || (info.st_uid != geteuid() && info.st_uid != 0) ||
      (info.st_gid != getegid() && info.st_gid != 0)) {
    if (err) *err = "旧共享帧文件身份不可信";
    return true;
  }
  const int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
  if (fd < 0) { if (err) *err = std::strerror(errno); return true; }
  uint64_t magic = 0, heartbeat = 0;
  uint32_t version = 0, writer_pid = 0;
  const bool ok =
      read_field(fd, static_cast<off_t>(offsetof(SharedFrameHeader, magic)),
                 &magic, sizeof(magic)) &&
      read_field(fd, static_cast<off_t>(offsetof(SharedFrameHeader, version)),
                 &version, sizeof(version)) &&
      read_field(fd, static_cast<off_t>(offsetof(SharedFrameHeader, writer_pid)),
                 &writer_pid, sizeof(writer_pid)) &&
      read_field(fd, static_cast<off_t>(offsetof(SharedFrameHeader, heartbeat_ns)),
                 &heartbeat, sizeof(heartbeat));
  ::close(fd);
  if (!ok || magic != kSharedFrameMagic || version != kSharedFrameVersion) return false;
  if (!pid_alive(writer_pid)) return false;
  const uint64_t now = monotonic_ns();
  return heartbeat != 0 && now > heartbeat && now - heartbeat < 2000000000ULL;
}

}  // namespace

size_t SharedFrameLayout::frame_bytes() const {
  return static_cast<size_t>(stride) * static_cast<size_t>(height);
}
size_t SharedFrameLayout::slot_stride() const {
  return (frame_bytes() + 4095) & ~static_cast<size_t>(4095);
}
size_t SharedFrameLayout::total_size() const {
  return kSharedFrameHeaderSize + kSharedFrameSlotCount * slot_stride();
}
size_t SharedFrameLayout::slot_offset(uint32_t slot) const {
  return kSharedFrameHeaderSize + static_cast<size_t>(slot) * slot_stride();
}
bool validate_shared_layout(const SharedFrameLayout& layout, std::string* err) {
  if (layout.width != 1920 || layout.height != 1080 ||
      layout.format != kSharedFrameFormatXrgb8888 ||
      layout.stride < layout.width * 4 ||
      layout.total_size() > (size_t{1} << 34)) {
    if (err) *err = "共享帧布局无效";
    return false;
  }
  return true;
}

SharedFrameWriter::SharedFrameWriter(const SharedFrameLayout& layout)
    : layout_(layout) {}
SharedFrameWriter::~SharedFrameWriter() { close(); }

bool SharedFrameWriter::open(const std::string& path, std::string* err) {
  close();
  if (!validate_shared_layout(layout_, err)) return false;
  path_ = path;
  std::string active_err;
  if (target_writer_active(path, &active_err)) {
    if (err) *err = active_err;
    path_.clear();
    return false;
  }
  const int flags = O_CREAT | O_EXCL | O_RDWR | O_CLOEXEC | O_NOFOLLOW;
  const size_t total = layout_.total_size();
  for (int attempt = 0; attempt < 8 && fd_ < 0; ++attempt) {
    temp_path_ = path + ".tmp." + std::to_string(current_pid()) + "." +
                 std::to_string(monotonic_ns() + static_cast<uint64_t>(attempt));
    fd_ = ::open(temp_path_.c_str(), flags, 0660);
    if (fd_ < 0 && errno != EEXIST) break;
  }
  if (fd_ < 0) { if (err) *err = std::strerror(errno); close(); return false; }
  if (fchmod(fd_, 0660) != 0 ||
      ftruncate(fd_, static_cast<off_t>(total)) != 0) {
    if (err) *err = std::strerror(errno);
    close();
    return false;
  }
  mapping_ = mmap(nullptr, total, PROT_READ | PROT_WRITE, MAP_SHARED, fd_, 0);
  if (mapping_ == MAP_FAILED) {
    mapping_ = nullptr; if (err) *err = std::strerror(errno); close(); return false;
  }
  header_ = new (mapping_) SharedFrameHeader();
  header_->ready.store(0, std::memory_order_relaxed);
  header_->magic = kSharedFrameMagic;
  header_->version = kSharedFrameVersion;
  header_->header_size = kSharedFrameHeaderSize;
  header_->width = layout_.width;
  header_->height = layout_.height;
  header_->stride = layout_.stride;
  header_->format = layout_.format;
  header_->slot_count = kSharedFrameSlotCount;
  header_->slot_stride = layout_.slot_stride();
  header_->total_size = total;
  header_->generation = monotonic_ns() ^ static_cast<uint64_t>(current_pid());
  header_->heartbeat_ns.store(monotonic_ns(), std::memory_order_release);
  header_->writer_pid.store(current_pid(), std::memory_order_release);
  for (uint32_t i = 0; i < kSharedFrameSlotCount; ++i) {
    header_->slot_state[i].store(static_cast<uint32_t>(SlotState::Free),
                                 std::memory_order_release);
    header_->slot_seq[i].store(0, std::memory_order_relaxed);
    header_->slot_timestamp_ns[i].store(0, std::memory_order_relaxed);
    header_->slot_generation[i].store(0, std::memory_order_relaxed);
  }
  header_->latest_token.store(0, std::memory_order_release);
  header_->ready.store(1, std::memory_order_release);
  struct stat info {};
  if (fstat(fd_, &info) != 0 || ::rename(temp_path_.c_str(), path.c_str()) != 0) {
    if (err) *err = std::strerror(errno);
    close();
    return false;
  }
  temp_path_.clear();
  dev_ = static_cast<uint64_t>(info.st_dev);
  ino_ = static_cast<uint64_t>(info.st_ino);
  next_seq_ = 1;
  return true;
}

bool SharedFrameWriter::publish(const uint8_t* frame, int source_stride,
                                std::string* err) {
  if (!is_open() || frame == nullptr || source_stride <= 0 ||
      static_cast<size_t>(source_stride) < static_cast<size_t>(layout_.width) * 4) {
    if (err) *err = "共享帧写入参数无效";
    return false;
  }
  int slot = -1;
  for (int pass = 0; pass < 2 && slot < 0; ++pass) {
    const uint32_t expected =
        pass == 0 ? static_cast<uint32_t>(SlotState::Free)
                  : static_cast<uint32_t>(SlotState::Ready);
    for (uint32_t i = 0; i < kSharedFrameSlotCount; ++i) {
      if (leased_[i]) continue;
      uint32_t current = expected;
      if (header_->slot_state[i].compare_exchange_strong(
              current, static_cast<uint32_t>(SlotState::Writing),
              std::memory_order_acq_rel, std::memory_order_relaxed)) {
        slot = static_cast<int>(i);
        break;
      }
    }
  }
  if (slot < 0) {
    header_->dropped_frames.fetch_add(1, std::memory_order_relaxed);
    if (err) *err = "没有可发布共享帧的空闲槽";
    return false;
  }
  const uint64_t seq = next_seq_++;
  header_->slot_seq[slot].store(seq, std::memory_order_relaxed);
  header_->slot_timestamp_ns[slot].store(monotonic_ns(), std::memory_order_relaxed);
  header_->slot_generation[slot].store(
      static_cast<uint32_t>(header_->generation), std::memory_order_relaxed);
  uint8_t* destination = static_cast<uint8_t*>(mapping_) + layout_.slot_offset(slot);
  for (uint32_t row = 0; row < layout_.height; ++row) {
    std::memcpy(destination + static_cast<size_t>(row) * layout_.stride,
                frame + static_cast<size_t>(row) * source_stride,
                static_cast<size_t>(layout_.width) * 4);
  }
  header_->heartbeat_ns.store(monotonic_ns(), std::memory_order_release);
  header_->slot_state[slot].store(static_cast<uint32_t>(SlotState::Ready),
                                  std::memory_order_release);
  header_->latest_token.store((seq << 2) | static_cast<uint64_t>(slot),
                              std::memory_order_release);
  return true;
}

void SharedFrameWriter::heartbeat() {
  if (header_) header_->heartbeat_ns.store(monotonic_ns(), std::memory_order_release);
}
void SharedFrameWriter::invalidate() {
  if (!header_) return;
  header_->latest_token.store(0, std::memory_order_release);
  for (uint32_t i = 0; i < kSharedFrameSlotCount; ++i) {
    if (leased_[i]) continue;
    uint32_t current = static_cast<uint32_t>(SlotState::Ready);
    header_->slot_state[i].compare_exchange_strong(
        current, static_cast<uint32_t>(SlotState::Free),
        std::memory_order_acq_rel, std::memory_order_relaxed);
  }
  heartbeat();
}
void SharedFrameWriter::close() {
  if (header_) {
    header_->writer_pid.store(0, std::memory_order_release);
    header_->heartbeat_ns.store(0, std::memory_order_release);
  }
  if (mapping_) { munmap(mapping_, layout_.total_size()); mapping_ = nullptr; header_ = nullptr; }
  if (fd_ >= 0) { ::close(fd_); fd_ = -1; }
  if (!temp_path_.empty()) { ::unlink(temp_path_.c_str()); temp_path_.clear(); }
  if (!path_.empty()) {
    struct stat info {};
    if (dev_ != 0 && ino_ != 0 && ::lstat(path_.c_str(), &info) == 0 &&
        static_cast<uint64_t>(info.st_dev) == dev_ &&
        static_cast<uint64_t>(info.st_ino) == ino_) {
      ::unlink(path_.c_str());
    }
    path_.clear(); dev_ = 0; ino_ = 0;
  }
  for (bool& value : leased_) value = false;
}
bool SharedFrameWriter::is_open() const { return fd_ >= 0 && header_ != nullptr; }
uint64_t SharedFrameWriter::dropped_frames() const {
  return header_ ? header_->dropped_frames.load(std::memory_order_relaxed) : 0;
}
SharedFrameHeader* SharedFrameWriter::header() { return header_; }

bool SharedFrameWriter::lease_acquire(SharedLeaseInfo* info) {
  if (!is_open() || info == nullptr) return false;
  const uint64_t token = header_->latest_token.load(std::memory_order_acquire);
  if (token == 0) return false;
  const uint32_t slot = static_cast<uint32_t>(token & 0x3);
  const uint64_t seq = token >> 2;
  if (slot >= kSharedFrameSlotCount || leased_[slot]) return false;
  if (header_->slot_state[slot].load(std::memory_order_acquire) !=
      static_cast<uint32_t>(SlotState::Ready)) return false;
  if (header_->slot_seq[slot].load(std::memory_order_acquire) != seq) return false;
  leased_[slot] = true;
  header_->slot_state[slot].store(static_cast<uint32_t>(SlotState::Reading),
                                  std::memory_order_release);
  info->token = token; info->slot = slot; info->width = header_->width;
  info->height = header_->height; info->stride = header_->stride;
  info->seq = seq; info->generation = header_->generation;
  info->offset = layout_.slot_offset(slot);
  info->payload = static_cast<uint64_t>(header_->stride) * header_->height;
  return true;
}
bool SharedFrameWriter::lease_release(uint64_t token) {
  if (!is_open() || token == 0) return false;
  const uint32_t slot = static_cast<uint32_t>(token & 0x3);
  const uint64_t seq = token >> 2;
  if (slot >= kSharedFrameSlotCount || !leased_[slot]) return false;
  if (header_->slot_state[slot].load(std::memory_order_acquire) !=
          static_cast<uint32_t>(SlotState::Reading) ||
      header_->slot_seq[slot].load(std::memory_order_acquire) != seq) return false;
  leased_[slot] = false;
  header_->slot_state[slot].store(static_cast<uint32_t>(SlotState::Free),
                                  std::memory_order_release);
  return true;
}

SharedFrameReader::SharedFrameReader() = default;
SharedFrameReader::~SharedFrameReader() { close(); }
bool SharedFrameReader::open(const std::string& path, const std::string& socket_path,
                             std::string* err) {
  close();
  struct stat info {};
  if (::lstat(path.c_str(), &info) != 0 || !S_ISREG(info.st_mode) ||
      (info.st_mode & 0002) != 0 ||
      (info.st_uid != 0 && info.st_uid != geteuid())) {
    if (err) *err = "共享帧文件不可信或不存在";
    return false;
  }
  fd_ = ::open(path.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
  if (fd_ < 0) { if (err) *err = std::strerror(errno); return false; }
  struct stat fd_info {};
  if (fstat(fd_, &fd_info) != 0 || fd_info.st_dev != info.st_dev ||
      fd_info.st_ino != info.st_ino ||
      fd_info.st_size < static_cast<off_t>(kSharedFrameHeaderSize) ||
      fd_info.st_size > static_cast<off_t>(size_t{1} << 34)) {
    if (err) *err = "共享帧 inode 或长度无效";
    close();
    return false;
  }
  mapping_size_ = static_cast<size_t>(fd_info.st_size);
  mapping_ = mmap(nullptr, mapping_size_, PROT_READ, MAP_SHARED, fd_, 0);
  if (mapping_ == MAP_FAILED) {
    mapping_ = nullptr; if (err) *err = std::strerror(errno); close(); return false;
  }
  header_ = static_cast<SharedFrameHeader*>(mapping_);
  if (header_->magic != kSharedFrameMagic ||
      header_->ready.load(std::memory_order_acquire) != 1) {
    if (err) *err = "共享帧 magic/ready 不匹配";
    close();
    return false;
  }
  bool layout_ok = header_->version == kSharedFrameVersion &&
                   header_->header_size == kSharedFrameHeaderSize &&
                   header_->slot_count == kSharedFrameSlotCount &&
                   header_->width == 1920 && header_->height == 1080 &&
                   header_->format == kSharedFrameFormatXrgb8888 &&
                   header_->stride >= 1920ULL * 4 &&
                   header_->stride <= (1ULL << 20) &&
                   header_->slot_stride <= (1ULL << 34) &&
                   header_->total_size <= mapping_size_;
  if (layout_ok) {
    const uint64_t payload = static_cast<uint64_t>(header_->stride) * header_->height;
    const uint64_t expected = static_cast<uint64_t>(header_->header_size) +
                              static_cast<uint64_t>(header_->slot_count) *
                                  header_->slot_stride;
    layout_ok = header_->slot_stride >= payload && expected == header_->total_size;
    for (uint32_t slot = 0; slot < header_->slot_count && layout_ok; ++slot) {
      if (static_cast<uint64_t>(header_->header_size) +
              static_cast<uint64_t>(slot) * header_->slot_stride + payload >
          mapping_size_) layout_ok = false;
    }
  }
  if (!layout_ok) { if (err) *err = "共享帧布局无效"; close(); return false; }
  client_.reset(new FrameLeaseClient());
  if (!client_->connect(socket_path, err)) { close(); return false; }
  path_ = path;
  return true;
}
FrameReadResult SharedFrameReader::read(SharedFrameView* frame, std::string* err) {
  if (!is_open() || !client_ || frame == nullptr) {
    if (err) *err = "共享帧未打开";
    return FrameReadResult::Error;
  }
  if (file_replaced(path_)) {
    client_->close();
    if (err) *err = "共享帧 inode 已变化";
    return FrameReadResult::Error;
  }
  LeaseReply reply;
  const LeaseResult result = client_->acquire(&reply, err);
  if (result == LeaseResult::Empty) return FrameReadResult::NoFrame;
  if (result == LeaseResult::Error) return FrameReadResult::Error;
  if (file_replaced(path_)) {
    client_->release(reply.token, nullptr);
    client_->close();
    if (err) *err = "共享帧在租约期间发生 inode 变化";
    return FrameReadResult::Error;
  }
  const uint64_t payload = static_cast<uint64_t>(header_->stride) * header_->height;
  const bool valid = reply.slot < kSharedFrameSlotCount &&
                     reply.width == header_->width &&
                     reply.height == header_->height &&
                     reply.stride == header_->stride &&
                     reply.payload == payload &&
                     header_->slot_stride != 0 &&
                     reply.offset ==
                         kSharedFrameHeaderSize +
                             static_cast<uint64_t>(reply.slot) *
                                 header_->slot_stride &&
                     reply.offset + payload <= mapping_size_ &&
                     reply.seq == header_->slot_seq[reply.slot].load(
                                        std::memory_order_acquire) &&
                     reply.generation == header_->generation;
  if (!valid) {
    client_->release(reply.token, nullptr);
    client_->close();
    if (err) *err = "共享租约布局/seq/generation 不匹配";
    return FrameReadResult::Error;
  }
  const uint8_t* source = static_cast<const uint8_t*>(mapping_) + reply.offset;
  frame->data.resize(payload);
  std::memcpy(frame->data.data(), source, payload);
  frame->width = header_->width;
  frame->height = header_->height;
  frame->stride = header_->stride;
  frame->format = header_->format;
  frame->seq = reply.seq;
  frame->generation = reply.generation;
  frame->timestamp_ns = header_->slot_timestamp_ns[reply.slot].load(
      std::memory_order_acquire);
  if (!client_->release(reply.token, err)) return FrameReadResult::Error;
  return FrameReadResult::Ok;
}
bool SharedFrameReader::file_replaced(const std::string& path) const {
  if (fd_ < 0) return true;
  struct stat fd_info {}, path_info {};
  if (fstat(fd_, &fd_info) != 0 || ::lstat(path.c_str(), &path_info) != 0) return true;
  return fd_info.st_dev != path_info.st_dev || fd_info.st_ino != path_info.st_ino;
}
bool SharedFrameReader::socket_connected() const { return client_ && client_->connected(); }
uint64_t SharedFrameReader::generation() const { return header_ ? header_->generation : 0; }
uint64_t SharedFrameReader::heartbeat_age_ms() const {
  if (!header_) return UINT64_MAX;
  const uint64_t heartbeat = header_->heartbeat_ns.load(std::memory_order_acquire);
  if (heartbeat == 0) return UINT64_MAX;
  const uint64_t now = monotonic_ns();
  return now > heartbeat ? (now - heartbeat) / 1000000ULL : 0;
}
void SharedFrameReader::close() {
  client_.reset();
  if (mapping_) { munmap(mapping_, mapping_size_); mapping_ = nullptr; mapping_size_ = 0; header_ = nullptr; }
  if (fd_ >= 0) { ::close(fd_); fd_ = -1; }
  path_.clear();
}
bool SharedFrameReader::is_open() const { return fd_ >= 0 && header_ != nullptr && client_; }
}}  // namespace
