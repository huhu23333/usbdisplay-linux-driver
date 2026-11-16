// SPDX-License-Identifier: MIT
// Media packetization / depacketization.
//
// The receiver protocol is still being reversed (see work/proto/PROTOCOL.md
// once it lands). This header intentionally exposes the wire codec so the
// tools/tests can use it directly and so the exact format can be replaced in
// one place (src/proto/wire.cpp + packetizer.cpp) without touching the
// Controller API.
//
// WireFormat::LegacyEzV1 is the provisional layout documented in README.md
// ("Protocol assumptions"). It is a 32-byte little-endian header followed by
// a slice of one access unit:
//
//   off size field
//   0    4   magic "EZDP" (bytes 45 5A 44 50)
//   4    1   version = 1
//   5    1   kind: 1=video 2=audio 3=config(JSON)
//   6    1   flags: bit0 keyframe, bit1 AU start, bit2 AU end,
//                    bit3 config, bit4 discontinuity
//   7    1   stream id (0=video, 1=audio)
//   8    4   packet sequence (wraps)
//  12    4   frame id (access unit counter, wraps)
//  16    2   split count (packets in this access unit)
//  18    2   split position (0-based)
//  20    4   access unit size in bytes (all splits)
//  24    8   presentation timestamp, microseconds, unsigned
//  32    0   payload
//
// WireFormat::RtpAvc is standard RTP (RFC 3550) carrying H.264 per RFC 6184
// (single NAL unit, STAP-A and FU-A) with a 90 kHz timestamp. It is a
// fallback/profile because the Windows binary contains "-rtp" strings and an
// RTSP implementation; it is NOT confirmed to be what the device accepts.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace usbdisplay {
namespace proto {

constexpr size_t kLegacyHeaderSize = 32;
constexpr uint32_t kLegacyMagic = 0x50445A45u;  // "EZDP" little-endian
constexpr size_t kTcpLengthPrefixSize = 4;

// PROVISIONAL port defaults. The real ports have NOT been confirmed by the
// reverse engineering yet; every port is overridable via DeviceSelector.
// 192.168.203.1 / 192.168.168.1 are the receiver addresses seen in the
// Windows binary. Update this block when work/proto/PROTOCOL.md lands.
constexpr uint16_t kProvisionalMediaTcpPort = 8000;
constexpr uint16_t kProvisionalVideoUdpPort = 8000;
constexpr uint16_t kProvisionalAudioUdpPort = 8002;
constexpr uint16_t kProvisionalControlTcpPort = 8001;

enum class WireFormat : uint8_t {
  LegacyEzV1 = 0,
  RtpAvc = 1,
};

const char* wire_format_name(WireFormat fmt);
bool wire_format_from_name(const std::string& name, WireFormat* out);

enum class MediaKind : uint8_t {
  Video = 1,
  Audio = 2,
  Config = 3,
};

enum PacketFlags : uint8_t {
  kFlagKeyframe = 1u << 0,
  kFlagAuStart = 1u << 1,
  kFlagAuEnd = 1u << 2,
  kFlagConfig = 1u << 3,
  kFlagDiscontinuity = 1u << 4,
};

struct PacketizeOptions {
  WireFormat format = WireFormat::LegacyEzV1;
  // Max media payload bytes per packet. Must stay below the path MTU for UDP
  // (the device network is an USB gadget: 1400 is a safe default).
  size_t max_payload = 1400;
  uint8_t stream_id = 0;
  uint32_t ssrc = 0x455A4D52u;  // "EZMR"
  uint8_t rtp_payload_type = 96;
  // Legacy format over TCP: prefix every packet with a 4-byte big-endian
  // length. Ignored for RTP (which is datagram oriented).
  bool length_prefix = false;
};

// A parsed media packet. payload points into the input buffer and is valid
// only while the input stays alive.
struct ParsedPacket {
  WireFormat format = WireFormat::LegacyEzV1;
  MediaKind kind = MediaKind::Video;
  uint8_t flags = 0;
  uint8_t stream_id = 0;
  uint32_t seq = 0;
  uint32_t frame_id = 0;
  uint16_t split_count = 1;
  uint16_t split_position = 0;
  uint32_t au_size = 0;
  uint64_t pts_us = 0;
  bool keyframe = false;
  const uint8_t* payload = nullptr;
  size_t payload_len = 0;

  // RTP-specific.
  uint16_t rtp_seq = 0;
  uint32_t rtp_timestamp = 0;
  uint32_t ssrc = 0;
  bool marker = false;
};

class Packetizer {
 public:
  Packetizer();
  explicit Packetizer(const PacketizeOptions& options);

  void set_options(const PacketizeOptions& options);
  const PacketizeOptions& options() const { return options_; }
  void reset();

  uint32_t next_seq() const { return seq_; }

  // Split one access unit (raw bytes, e.g. an H.264 Annex-B AU) or one audio
  // payload into wire packets appended to `out`. frame_id must increase per
  // access unit; pts_us is in microseconds.
  void packetize(const uint8_t* data, size_t len, MediaKind kind,
                 uint64_t pts_us, bool keyframe, uint32_t frame_id,
                 std::vector<std::vector<uint8_t>>& out);

  // Convenience wrapper: legacy config packet carrying a UTF-8 JSON body.
  void packetize_config(const std::string& json, uint64_t pts_us,
                        std::vector<std::vector<uint8_t>>& out);

  // TCP length-prefix helpers (big-endian u32).
  static void append_length_prefix(std::vector<uint8_t>& packet);
  // Returns true when a complete frame is available. consumed is set to the
  // number of bytes used from data (prefix + payload).
  static bool strip_length_prefix(const uint8_t* data, size_t len,
                                  size_t* consumed, const uint8_t** payload,
                                  size_t* payload_len);

 private:
  PacketizeOptions options_;
  uint32_t seq_ = 0;
  uint32_t rtp_timestamp_base_ = 0;
};

class Depacketizer {
 public:
  explicit Depacketizer(WireFormat format = WireFormat::LegacyEzV1);
  void set_format(WireFormat format) { format_ = format; }

  // Parse exactly one wire packet (length prefix already stripped for TCP).
  bool parse(const uint8_t* packet, size_t len, ParsedPacket* out,
             std::string* err) const;

 private:
  WireFormat format_;
};

// Annex-B / access unit helpers (H.264 only; HEVC support is future work).
namespace annexb {

struct NalUnit {
  const uint8_t* data = nullptr;  // includes the start code
  size_t size = 0;
  uint8_t type = 0;   // nal_unit_type (data[4] or data[3] & 0x1f)
  bool is_vcl = false;
  bool is_keyframe = false;  // IDR (5) or SPS/PPS ahead of it
};

struct AccessUnit {
  const uint8_t* data = nullptr;
  size_t size = 0;
  uint64_t pts_us = 0;
  bool keyframe = false;
  std::vector<NalUnit> nals;
};

// Split an Annex-B elementary stream into access units. The parser uses
// AUD (9) / first_mb_in_slice==0 boundaries and attaches leading SPS/PPS/SEI
// to the following picture. Returns false on malformed input.
bool split_access_units(const uint8_t* data, size_t len,
                        std::vector<AccessUnit>* out, std::string* err);

// Return NAL unit types present in an access unit (for diagnostics).
std::string describe_access_unit(const AccessUnit& au);

}  // namespace annexb

}  // namespace proto
}  // namespace usbdisplay
