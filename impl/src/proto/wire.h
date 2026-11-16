// SPDX-License-Identifier: MIT
// 打包、解包与模拟设备共用的报文编解码工具。
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

#include "usbdisplay/protocol.h"

namespace usbdisplay {
namespace proto {
namespace wire {

void write_legacy_header(uint8_t* p, MediaKind kind, uint8_t flags,
                         uint8_t stream_id, uint32_t seq, uint32_t frame_id,
                         uint16_t split_count, uint16_t split_position,
                         uint32_t au_size, uint64_t pts_us);

bool parse_legacy_header(const uint8_t* p, size_t len, ParsedPacket* out,
                         std::string* err);

void write_rtp_header(uint8_t* p, bool marker, uint8_t payload_type,
                      uint16_t seq, uint32_t timestamp, uint32_t ssrc);

bool parse_rtp_header(const uint8_t* p, size_t len, ParsedPacket* out,
                      std::string* err);

}  // namespace wire
}  // namespace proto
}  // namespace usbdisplay
