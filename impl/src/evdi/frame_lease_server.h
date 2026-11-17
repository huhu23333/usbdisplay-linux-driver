// SPDX-License-Identifier: MIT
#pragma once
#include <cstdint>
#include <cstddef>
#include <string>
#include <vector>
#include "evdi/frame_lease_protocol.h"
#include "evdi/shared_frame.h"
namespace usbdisplay { namespace evdi {
class FrameLeaseServer {
 public:
  explicit FrameLeaseServer(SharedFrameWriter* writer);
  ~FrameLeaseServer();
  FrameLeaseServer(const FrameLeaseServer&) = delete;
  FrameLeaseServer& operator=(const FrameLeaseServer&) = delete;
  bool listen(const std::string& path, std::string* err);
  void poll();
  void close();
  bool listening() const;
  // 测试用：可缩短空闲/半包/租约超时。
  void set_timeouts(int idle_ms, int half_ms, int lease_ms);
  size_t pending_output() const;
 private:
  void close_client();
  void check_timeouts(uint64_t now_ms);
  void flush_output();
  void process_request(const uint8_t* data);
  SharedFrameWriter* writer_;
  std::string path_;
  int listen_fd_ = -1;
  int client_fd_ = -1;
  uint8_t input_[kLeaseRequestSize]{};
  size_t input_size_ = 0;
  bool lease_active_ = false;
  uint64_t lease_token_ = 0;
  uint64_t last_activity_ms_ = 0;
  uint64_t lease_started_ms_ = 0;
  int idle_timeout_ms_ = 15000;
  int half_packet_timeout_ms_ = 15000;
  int lease_timeout_ms_ = 3000;
  std::vector<uint8_t> output_;
};
}}  // namespace
