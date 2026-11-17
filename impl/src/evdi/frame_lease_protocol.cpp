// SPDX-License-Identifier: MIT
#include "evdi/frame_lease_protocol.h"

#include <cstring>

namespace usbdisplay {
namespace evdi {
namespace {

void put16(uint8_t* out, size_t offset, uint16_t value) {
  out[offset] = static_cast<uint8_t>(value & 0xff);
  out[offset + 1] = static_cast<uint8_t>((value >> 8) & 0xff);
}
void put32(uint8_t* out, size_t offset, uint32_t value) {
  for (int i = 0; i < 4; ++i) {
    out[offset + i] = static_cast<uint8_t>((value >> (8 * i)) & 0xff);
  }
}
void put64(uint8_t* out, size_t offset, uint64_t value) {
  for (int i = 0; i < 8; ++i) {
    out[offset + i] = static_cast<uint8_t>((value >> (8 * i)) & 0xff);
  }
}
uint16_t get16(const uint8_t* in, size_t offset) {
  return static_cast<uint16_t>(in[offset] | (in[offset + 1] << 8));
}
uint32_t get32(const uint8_t* in, size_t offset) {
  uint32_t value = 0;
  for (int i = 0; i < 4; ++i) value |= static_cast<uint32_t>(in[offset + i]) << (8 * i);
  return value;
}
uint64_t get64(const uint8_t* in, size_t offset) {
  uint64_t value = 0;
  for (int i = 0; i < 8; ++i) value |= static_cast<uint64_t>(in[offset + i]) << (8 * i);
  return value;
}

}  // namespace

void encode_lease_request(const LeaseRequest& request,
                          uint8_t out[kLeaseRequestSize]) {
  std::memset(out, 0, kLeaseRequestSize);
  out[0] = static_cast<uint8_t>(request.command);
  out[1] = kLeaseProtocolVersion;
  put16(out, 2, kLeaseRequestSize);
  put64(out, 4, request.token);
}

bool decode_lease_request(const uint8_t in[kLeaseRequestSize],
                          LeaseRequest* request) {
  if (request == nullptr || in[1] != kLeaseProtocolVersion ||
      get16(in, 2) != kLeaseRequestSize) {
    return false;
  }
  const uint8_t command = in[0];
  if (command != static_cast<uint8_t>(LeaseCommand::Acquire) &&
      command != static_cast<uint8_t>(LeaseCommand::Release)) {
    return false;
  }
  request->command = static_cast<LeaseCommand>(command);
  request->token = get64(in, 4);
  return true;
}

void encode_lease_reply(const LeaseReply& reply,
                        uint8_t out[kLeaseReplySize]) {
  std::memset(out, 0, kLeaseReplySize);
  out[0] = static_cast<uint8_t>(reply.status);
  out[1] = kLeaseProtocolVersion;
  put16(out, 2, kLeaseReplySize);
  put64(out, 4, reply.token);
  put32(out, 12, reply.slot);
  put32(out, 16, reply.width);
  put32(out, 20, reply.height);
  put32(out, 24, reply.stride);
  put64(out, 28, reply.seq);
  put64(out, 36, reply.generation);
  put64(out, 44, reply.offset);
  put64(out, 52, reply.payload);
}

bool decode_lease_reply(const uint8_t in[kLeaseReplySize], LeaseReply* reply) {
  if (reply == nullptr || in[1] != kLeaseProtocolVersion ||
      get16(in, 2) != kLeaseReplySize) {
    return false;
  }
  reply->status = static_cast<LeaseStatus>(in[0]);
  reply->token = get64(in, 4);
  reply->slot = get32(in, 12);
  reply->width = get32(in, 16);
  reply->height = get32(in, 20);
  reply->stride = get32(in, 24);
  reply->seq = get64(in, 28);
  reply->generation = get64(in, 36);
  reply->offset = get64(in, 44);
  reply->payload = get64(in, 52);
  return true;
}

}  // namespace evdi
}  // namespace usbdisplay
