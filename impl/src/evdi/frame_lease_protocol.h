// SPDX-License-Identifier: MIT
// 共享帧租约 socket 固定小端协议。
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

namespace usbdisplay {
namespace evdi {

constexpr uint8_t kLeaseProtocolVersion = 1;
constexpr size_t kLeaseRequestSize = 12;
constexpr size_t kLeaseReplySize = 64;

enum class LeaseCommand : uint8_t {
  Acquire = 1,
  Release = 2,
};

enum class LeaseStatus : uint8_t {
  Ok = 0,
  Empty = 1,
  Stale = 2,
  Error = 3,
};

struct LeaseRequest {
  LeaseCommand command = LeaseCommand::Acquire;
  uint64_t token = 0;  // Acquire 时忽略；Release 时校验
};

struct LeaseReply {
  LeaseStatus status = LeaseStatus::Error;
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

void encode_lease_request(const LeaseRequest& request, uint8_t out[kLeaseRequestSize]);
bool decode_lease_request(const uint8_t in[kLeaseRequestSize],
                          LeaseRequest* request);
void encode_lease_reply(const LeaseReply& reply, uint8_t out[kLeaseReplySize]);
bool decode_lease_reply(const uint8_t in[kLeaseReplySize], LeaseReply* reply);

}  // namespace evdi
}  // namespace usbdisplay
