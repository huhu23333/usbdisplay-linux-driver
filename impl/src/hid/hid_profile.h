// SPDX-License-Identifier: MIT
// 接收器控制通道的 HID 报告封装。
//
// 设备提供两个 HID 接口：
// IF0：报告编号 1，报告长度 4096 字节，用于控制通道（JSON-RPC 命令）。
// IF1：视频通道通过 libusb 发送 4096 字节 URB，不使用 HID 报告。
//
// 控制消息由一个或多个固定长度 HID 报告组成，每个报告的 16 字节头部：
// [0] 报告编号
// [1..4] 大端序，本报告负载字节数
// [5..8] 大端序参数三
// [9..12] 大端序参数四
// [13..15] 保留，取值为零
// [16..] 负载，长度不超过报告长度减 16
//
// 设备发往主机的报告结构相同，负载为 JSON 文本。
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
  // 第 13 到 15 字节保持为零
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

// 早期网络传输使用的外层头部构建函数，当前代码不使用。
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

// 早期网络传输的视频消息构建函数，当前 USB 视频通道不使用。
// include_payload_len 为真时在偏移 32 处追加四字节负载长度。
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
