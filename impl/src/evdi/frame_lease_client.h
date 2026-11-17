// SPDX-License-Identifier: MIT
#pragma once
#include <cstdint>
#include <string>
#include "evdi/frame_lease_protocol.h"
namespace usbdisplay { namespace evdi {
enum class LeaseResult { Ok, Empty, Error };

class FrameLeaseClient {
 public:
  FrameLeaseClient() = default;
  ~FrameLeaseClient();
  FrameLeaseClient(const FrameLeaseClient&) = delete;
  FrameLeaseClient& operator=(const FrameLeaseClient&) = delete;
  bool connect(const std::string& path, std::string* err);
  LeaseResult acquire(LeaseReply* reply, std::string* err);
  bool release(uint64_t token, std::string* err);
  bool connected() const;
  void close();
 private:
  int fd_ = -1;
};
}}  // namespace
