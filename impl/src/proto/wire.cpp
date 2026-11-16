// SPDX-License-Identifier: MIT
#include "proto/wire.h"

#include <cstring>

#include "common/util.h"

namespace usbdisplay {
namespace proto {
namespace wire {

void write_legacy_header(uint8_t* p, MediaKind kind, uint8_t flags,
                         uint8_t stream_id, uint32_t seq, uint32_t frame_id,
                         uint16_t split_count, uint16_t split_position,
                         uint32_t au_size, uint64_t pts_us) {
  std::memset(p, 0, kLegacyHeaderSize);
  write_le32(p + 0, kLegacyMagic);
  p[4] = 1;  // version
  p[5] = static_cast<uint8_t>(kind);
  p[6] = flags;
  p[7] = stream_id;
  write_le32(p + 8, seq);
  write_le32(p + 12, frame_id);
  write_le16(p + 16, split_count);
  write_le16(p + 18, split_position);
  write_le32(p + 20, au_size);
  write_le64(p + 24, pts_us);
}

bool parse_legacy_header(const uint8_t* p, size_t len, ParsedPacket* out,
                         std::string* err) {
  if (len < kLegacyHeaderSize) {
    if (err) *err = "legacy: packet shorter than 32-byte header";
    return false;
  }
  if (read_le32(p) != kLegacyMagic) {
    if (err) *err = "legacy: bad magic";
    return false;
  }
  if (p[4] != 1) {
    if (err) *err = "legacy: unsupported version " + std::to_string(p[4]);
    return false;
  }
  const uint8_t kind = p[5];
  if (kind < 1 || kind > 3) {
    if (err) *err = "legacy: bad media kind " + std::to_string(kind);
    return false;
  }
  out->format = WireFormat::LegacyEzV1;
  out->kind = static_cast<MediaKind>(kind);
  out->flags = p[6];
  out->stream_id = p[7];
  out->seq = read_le32(p + 8);
  out->frame_id = read_le32(p + 12);
  out->split_count = read_le16(p + 16);
  out->split_position = read_le16(p + 18);
  out->au_size = read_le32(p + 20);
  out->pts_us = read_le64(p + 24);
  out->keyframe = (out->flags & kFlagKeyframe) != 0;
  out->payload = p + kLegacyHeaderSize;
  out->payload_len = len - kLegacyHeaderSize;
  if (out->split_count != 0 && out->split_position >= out->split_count) {
    if (err) {
      *err = "legacy: split position " + std::to_string(out->split_position) +
             " >= count " + std::to_string(out->split_count);
    }
    return false;
  }
  return true;
}

void write_rtp_header(uint8_t* p, bool marker, uint8_t payload_type,
                      uint16_t seq, uint32_t timestamp, uint32_t ssrc) {
  p[0] = 0x80;  // V=2, P=0, X=0, CC=0
  p[1] = static_cast<uint8_t>((marker ? 0x80 : 0) | (payload_type & 0x7f));
  write_be16(p + 2, seq);
  write_be32(p + 4, timestamp);
  write_be32(p + 8, ssrc);
}

bool parse_rtp_header(const uint8_t* p, size_t len, ParsedPacket* out,
                      std::string* err) {
  if (len < 12) {
    if (err) *err = "rtp: packet shorter than 12-byte header";
    return false;
  }
  if ((p[0] >> 6) != 2) {
    if (err) *err = "rtp: unsupported version";
    return false;
  }
  const size_t cc = p[0] & 0x0f;
  size_t header = 12 + cc * 4;
  if ((p[0] & 0x10) != 0) {  // extension
    if (len < header + 4) {
      if (err) *err = "rtp: truncated extension header";
      return false;
    }
    const size_t ext_words = read_be16(p + header + 2);
    header += 4 + ext_words * 4;
  }
  if (len < header) {
    if (err) *err = "rtp: truncated header";
    return false;
  }
  size_t payload_end = len;
  if ((p[0] & 0x20) != 0) {  // padding
    const uint8_t pad = p[len - 1];
    if (pad == 0 || pad > len - header) {
      if (err) *err = "rtp: invalid padding";
      return false;
    }
    payload_end = len - pad;
  }
  out->format = WireFormat::RtpAvc;
  out->marker = (p[1] & 0x80) != 0;
  out->rtp_seq = read_be16(p + 2);
  out->rtp_timestamp = read_be32(p + 4);
  out->ssrc = read_be32(p + 8);
  out->seq = out->rtp_seq;
  out->pts_us = static_cast<uint64_t>(out->rtp_timestamp) * 1000000ULL / 90000ULL;
  out->payload = p + header;
  out->payload_len = payload_end - header;
  return true;
}

}  // namespace wire
}  // namespace proto
}  // namespace usbdisplay

namespace usbdisplay {
namespace proto {

const char* wire_format_name(WireFormat fmt) {
  switch (fmt) {
    case WireFormat::LegacyEzV1: return "legacy-v1";
    case WireFormat::RtpAvc: return "rtp";
  }
  return "unknown";
}

bool wire_format_from_name(const std::string& name, WireFormat* out) {
  if (name == "legacy-v1" || name == "legacy" || name == "ezdp") {
    if (out) *out = WireFormat::LegacyEzV1;
    return true;
  }
  if (name == "rtp" || name == "rtp-avc") {
    if (out) *out = WireFormat::RtpAvc;
    return true;
  }
  return false;
}

}  // namespace proto
}  // namespace usbdisplay

