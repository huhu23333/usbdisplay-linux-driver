// SPDX-License-Identifier: MIT
// USB HID report framing for the Actions Micro USB Display receiver.
//
// The device exposes two HID interfaces (verified from the HID report
// descriptors of a real 1de1:f201 unit):
//   IF0: report ID 1, 4095 data bytes (hidraw write buffer = 4096 bytes)
//        => control channel (JSON-RPC commands), endpoint 64 B
//   IF1: report ID 2, 12287 data bytes (buffer = 12288 bytes)
//        => video channel (H.264 access units), endpoint 768 B HB
//
// Every host->device message is a sequence of fixed-size HID output reports.
// The 16-byte header was verified on real hardware (work/hw/live_sender.py,
// probe_hid2.py): every report carries the header, and the big-endian length
// field is the number of payload bytes in *this* report:
//
//   off size field
//   0    1   report id (1 for IF0, 2 for IF1)
//   1    4   big-endian payload length of this report
//   5    4   big-endian param3
//   9    4   big-endian param4
//  13    3   zero padding
//  16    N   payload (N <= report_size - 16)
//
// Device->host reports use the same layout (length at offset 1, JSON at 16).
// A logical message (e.g. one video access unit) is split across reports; each
// report repeats the header with its own chunk length.
//
// Two video framings are implemented:
//   app   (default): 16-byte outer header + 32-byte LE inner header + Annex-B
//   ugreen (mode B): 14-byte LE outer header + 32-byte 'RRIM'/'TADV' header +
//                    Annex-B (still being reverse engineered, configurable)
//
// Video messages carry a 32-byte inner header in their payload:
//   [0]  u32 LE sequence number
//   [4]  u32 LE payload_len + 24  (i.e. total inner message - 8)
//   [8]  u32 LE message type
//   [12] u16 LE 0x1000, [14] u16 LE 0
//   [16] u32 LE 1
//   [20] u32 LE a
//   [24] u32 LE b
//   [28] u32 LE c
//   [32] ... Annex-B H.264 access unit
//
// All values are configurable through DeviceSelector so they can be tuned
// live once proto-re finalizes the spec.
#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include "common/util.h"

namespace usbdisplay {
namespace hid {

constexpr size_t kOuterHeaderSize = 16;
constexpr size_t kInnerHeaderSize = 32;
constexpr uint16_t kInnerMagic = 0x1000;

struct OuterHeader {
  uint8_t report_id = 0;
  uint32_t length = 0;  // payload bytes carried by this report
  uint32_t param3 = 0;
  uint32_t param4 = 0;
};

inline void write_outer_header(uint8_t* p, uint8_t report_id,
                               size_t payload_len, uint32_t param3,
                               uint32_t param4) {
  std::memset(p, 0, kOuterHeaderSize);
  p[0] = report_id;
  write_be32(p + 1, static_cast<uint32_t>(payload_len));
  write_be32(p + 5, param3);
  write_be32(p + 9, param4);
  // bytes 13..15 stay zero
}

inline bool parse_outer_header(const uint8_t* p, size_t len, OuterHeader* out,
                               std::string* err) {
  if (len < kOuterHeaderSize) {
    if (err) *err = "outer header truncated";
    return false;
  }
  out->report_id = p[0];
  out->length = read_be32(p + 1);
  out->param3 = read_be32(p + 5);
  out->param4 = read_be32(p + 9);
  return true;
}

struct InnerHeader {
  uint32_t seq = 0;
  uint32_t length = 0;  // payload_len + 24
  uint32_t msg_type = 0;
  uint16_t magic = kInnerMagic;
  uint16_t reserved = 0;
  uint32_t one = 1;
  uint32_t a = 0;
  uint32_t b = 0;
  uint32_t c = 0;
};

inline void write_inner_header(uint8_t* p, const InnerHeader& header) {
  write_le32(p + 0, header.seq);
  write_le32(p + 4, header.length);
  write_le32(p + 8, header.msg_type);
  write_le16(p + 12, header.magic);
  write_le16(p + 14, header.reserved);
  write_le32(p + 16, header.one);
  write_le32(p + 20, header.a);
  write_le32(p + 24, header.b);
  write_le32(p + 28, header.c);
}

inline bool parse_inner_header(const uint8_t* p, size_t len, InnerHeader* out,
                               std::string* err) {
  if (len < kInnerHeaderSize) {
    if (err) *err = "inner header truncated";
    return false;
  }
  out->seq = read_le32(p + 0);
  out->length = read_le32(p + 4);
  out->msg_type = read_le32(p + 8);
  out->magic = read_le16(p + 12);
  out->reserved = read_le16(p + 14);
  out->one = read_le32(p + 16);
  out->a = read_le32(p + 20);
  out->b = read_le32(p + 24);
  out->c = read_le32(p + 28);
  return true;
}

// Ugreen / "mode B" inner header (provisional; reverse engineering ongoing):
//   [0..4)  magic: "RRIM" or "TADV"
//   [4..8)  u32 LE length (payload_len + 24, same convention as app mode)
//   [8..12) u32 LE message type
//   [12..16) u32 LE sequence
//   [16..20) u32 LE a
//   [20..24) u32 LE b
//   [24..28) u32 LE c
//   [28..32) u32 LE 0
inline std::vector<uint8_t> build_ugreen_video_message(
    const uint8_t* annexb, size_t len, uint32_t seq, uint32_t msg_type,
    const char magic[4], uint32_t a, uint32_t b, uint32_t c) {
  std::vector<uint8_t> message(kInnerHeaderSize + len);
  std::memcpy(message.data(), magic, 4);
  write_le32(message.data() + 4, static_cast<uint32_t>(len + 24));
  write_le32(message.data() + 8, msg_type);
  write_le32(message.data() + 12, seq);
  write_le32(message.data() + 16, a);
  write_le32(message.data() + 20, b);
  write_le32(message.data() + 24, c);
  write_le32(message.data() + 28, 0);
  if (len > 0) std::memcpy(message.data() + kInnerHeaderSize, annexb, len);
  return message;
}

// Video message builder: inner header + Annex-B access unit.
// With include_payload_len the TCP-builder layout is used: a trailing u32
// payload length at offset 32 (36-byte header, payload at 36).
inline std::vector<uint8_t> build_video_message(
    const uint8_t* annexb, size_t len, uint32_t seq, uint32_t msg_type,
    uint32_t field_one, uint32_t a, uint32_t b, uint32_t c, bool magic_1000,
    bool include_payload_len = false) {
  const size_t header_size =
      include_payload_len ? kInnerHeaderSize + 4 : kInnerHeaderSize;
  std::vector<uint8_t> message(header_size + len);
  InnerHeader header;
  header.seq = seq;
  header.length = static_cast<uint32_t>(len + 24);
  header.msg_type = msg_type;
  header.magic = magic_1000 ? kInnerMagic : 0;
  header.one = field_one;
  header.a = a;
  header.b = b;
  header.c = c;
  write_inner_header(message.data(), header);
  if (include_payload_len) {
    write_le32(message.data() + kInnerHeaderSize,
               static_cast<uint32_t>(len));
  }
  if (len > 0) std::memcpy(message.data() + header_size, annexb, len);
  return message;
}

}  // namespace hid
}  // namespace usbdisplay
