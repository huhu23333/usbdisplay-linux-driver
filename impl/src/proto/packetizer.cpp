// SPDX-License-Identifier: MIT
#include "usbdisplay/protocol.h"

#include <algorithm>
#include <cstring>

#include "common/util.h"
#include "proto/nal_scan.h"
#include "proto/wire.h"

namespace usbdisplay {
namespace proto {
namespace {

void append_prefix_if_needed(const PacketizeOptions& options,
                             std::vector<uint8_t>* packet) {
  if (!options.length_prefix) return;
  std::vector<uint8_t> framed;
  framed.resize(4 + packet->size());
  write_be32(framed.data(), static_cast<uint32_t>(packet->size()));
  std::memcpy(framed.data() + 4, packet->data(), packet->size());
  packet->swap(framed);
}

}  // namespace

Packetizer::Packetizer() = default;
Packetizer::Packetizer(const PacketizeOptions& options) : options_(options) {}

void Packetizer::set_options(const PacketizeOptions& options) {
  options_ = options;
}

void Packetizer::reset() {
  seq_ = 0;
  rtp_timestamp_base_ = 0;
}

void Packetizer::packetize(const uint8_t* data, size_t len, MediaKind kind,
                           uint64_t pts_us, bool keyframe, uint32_t frame_id,
                           std::vector<std::vector<uint8_t>>& out) {
  if (data == nullptr || len == 0) return;
  if (options_.max_payload == 0) options_.max_payload = 1;

  if (options_.format == WireFormat::LegacyEzV1) {
    size_t chunk = options_.max_payload;
    size_t count = (len + chunk - 1) / chunk;
    if (count > 0xFFFFu) {
      chunk = (len + 0xFFFEu) / 0xFFFFu;
      count = (len + chunk - 1) / chunk;
    }
    const uint16_t split_count = static_cast<uint16_t>(count);
    for (size_t i = 0; i < count; ++i) {
      const size_t offset = i * chunk;
      const size_t n = std::min(chunk, len - offset);
      uint8_t flags = 0;
      if (keyframe) flags |= kFlagKeyframe;
      if (i == 0) flags |= kFlagAuStart;
      if (i + 1 == count) flags |= kFlagAuEnd;
      std::vector<uint8_t> packet(kLegacyHeaderSize + n);
      wire::write_legacy_header(packet.data(), kind, flags, options_.stream_id,
                                seq_++, frame_id, split_count,
                                static_cast<uint16_t>(i),
                                static_cast<uint32_t>(len), pts_us);
      std::memcpy(packet.data() + kLegacyHeaderSize, data + offset, n);
      append_prefix_if_needed(options_, &packet);
      out.push_back(std::move(packet));
    }
    return;
  }

  // RTP 与 H.264 打包。
  std::vector<detail::NalRange> nals;
  const bool scanned = detail::scan_nal_units(data, len, &nals);
  struct RtpFrag {
    std::vector<uint8_t> nal;
    bool fu = false;
    bool fu_start = false;
    bool fu_end = false;
    uint8_t nal_type = 0;
    uint8_t nri = 0;
  };
  std::vector<RtpFrag> frags;
  if (!scanned) {
    // 允许输入为不带起始码的单个 NAL 单元。
    RtpFrag f;
    f.nal.assign(data, data + len);
    f.nal_type = static_cast<uint8_t>(data[0] & 0x1f);
    f.nri = static_cast<uint8_t>(data[0] & 0x60);
    frags.push_back(std::move(f));
  } else {
    // max_payload 为每个 UDP 报文的总负载上限，需扣除 12 字节 RTP 头部与 2 字节 FU-A 头。
    const size_t max_payload = std::max<size_t>(options_.max_payload, 15);
    const size_t rtp_budget = max_payload - 12;
    for (const auto& r : nals) {
      if (r.size == 0) continue;
      const uint8_t* nal = data + r.payload_offset;
      if (r.size <= rtp_budget) {
        RtpFrag f;
        f.nal.assign(nal, nal + r.size);
        f.nal_type = r.type;
        f.nri = static_cast<uint8_t>(nal[0] & 0x60);
        frags.push_back(std::move(f));
      } else {
        // FU-A 打包时原 NAL 头字节由 FU 指示与 FU 头替代，不包含在分片负载中。
        const uint8_t* body = nal + 1;
        const size_t body_size = r.size - 1;
        const size_t fu_capacity = rtp_budget - 2;
        const size_t count = (body_size + fu_capacity - 1) / fu_capacity;
        for (size_t i = 0; i < count; ++i) {
          const size_t off = i * fu_capacity;
          const size_t n = std::min(fu_capacity, body_size - off);
          RtpFrag f;
          f.fu = true;
          f.fu_start = (i == 0);
          f.fu_end = (i + 1 == count);
          f.nal_type = r.type;
          f.nri = static_cast<uint8_t>(nal[0] & 0x60);
          f.nal.assign(body + off, body + off + n);
          frags.push_back(std::move(f));
        }
      }
    }
  }
  if (frags.empty()) return;

  const uint32_t timestamp =
      static_cast<uint32_t>((pts_us * 9ULL) / 100ULL);  // 90 kHz
  for (size_t i = 0; i < frags.size(); ++i) {
    const RtpFrag& f = frags[i];
    const bool marker = (i + 1 == frags.size());
    std::vector<uint8_t> packet(12);
    wire::write_rtp_header(packet.data(), marker, options_.rtp_payload_type,
                           static_cast<uint16_t>(seq_++), timestamp,
                           options_.ssrc);
    if (!f.fu) {
      packet.insert(packet.end(), f.nal.begin(), f.nal.end());
    } else {
      packet.push_back(static_cast<uint8_t>(f.nri | 28));
      uint8_t fu_header = f.nal_type & 0x1f;
      if (f.fu_start) fu_header |= 0x80;
      if (f.fu_end) fu_header |= 0x40;
      packet.push_back(fu_header);
      packet.insert(packet.end(), f.nal.begin(), f.nal.end());
    }
    out.push_back(std::move(packet));
  }
  (void)kind;
  (void)keyframe;
  (void)frame_id;
}

void Packetizer::packetize_config(const std::string& json_body,
                                  uint64_t pts_us,
                                  std::vector<std::vector<uint8_t>>& out) {
  if (json_body.empty()) return;
  if (options_.format != WireFormat::LegacyEzV1) {
    // RTP 没有独立配置报文，流参数由控制通道传递。
    return;
  }
  const uint8_t flags =
      static_cast<uint8_t>(kFlagConfig | kFlagAuStart | kFlagAuEnd);
  std::vector<uint8_t> packet(kLegacyHeaderSize + json_body.size());
  wire::write_legacy_header(
      packet.data(), MediaKind::Config, flags, options_.stream_id, seq_++, 0,
      static_cast<uint16_t>(1), static_cast<uint16_t>(0),
      static_cast<uint32_t>(json_body.size()), pts_us);
  std::memcpy(packet.data() + kLegacyHeaderSize, json_body.data(),
              json_body.size());
  append_prefix_if_needed(options_, &packet);
  out.push_back(std::move(packet));
}

void Packetizer::append_length_prefix(std::vector<uint8_t>& packet) {
  std::vector<uint8_t> framed(4 + packet.size());
  write_be32(framed.data(), static_cast<uint32_t>(packet.size()));
  std::memcpy(framed.data() + 4, packet.data(), packet.size());
  packet.swap(framed);
}

bool Packetizer::strip_length_prefix(const uint8_t* data, size_t len,
                                     size_t* consumed, const uint8_t** payload,
                                     size_t* payload_len) {
  if (data == nullptr || len < 4) return false;
  const uint32_t n = read_be32(data);
  if (n == 0 || n > 64u * 1024 * 1024) return false;
  if (len < 4 + static_cast<size_t>(n)) return false;
  *payload = data + 4;
  *payload_len = n;
  *consumed = 4 + static_cast<size_t>(n);
  return true;
}

Depacketizer::Depacketizer(WireFormat format) : format_(format) {}

bool Depacketizer::parse(const uint8_t* packet, size_t len,
                         ParsedPacket* out, std::string* err) const {
  if (out == nullptr) return false;
  *out = ParsedPacket();
  if (format_ == WireFormat::RtpAvc) {
    ParsedPacket p;
    if (!wire::parse_rtp_header(packet, len, &p, err)) return false;
    if (p.payload_len == 0) {
      if (err) *err = "rtp: empty payload";
      return false;
    }
    const uint8_t* payload = p.payload;
    const size_t payload_len = p.payload_len;
    const uint8_t nal_type = static_cast<uint8_t>(payload[0] & 0x1f);
    p.kind = MediaKind::Video;
    p.stream_id = 0;
    if (nal_type >= 1 && nal_type <= 23) {
      p.split_count = 1;
      p.split_position = 0;
      p.flags = static_cast<uint8_t>(kFlagAuStart | kFlagAuEnd);
      p.keyframe = (nal_type == 5);
    } else if (nal_type == 24) {  // STAP-A
      p.payload = payload + 1;
      p.payload_len = payload_len - 1;
      p.split_count = 1;
      p.split_position = 0;
      p.flags = static_cast<uint8_t>(kFlagAuStart | kFlagAuEnd);
      p.keyframe = false;
    } else if (nal_type == 28) {  // FU-A
      if (payload_len < 2) {
        if (err) *err = "rtp: truncated FU-A";
        return false;
      }
      const uint8_t fu_header = payload[1];
      const uint8_t orig_type = static_cast<uint8_t>(fu_header & 0x1f);
      p.payload = payload + 2;
      p.payload_len = payload_len - 2;
      p.split_count = 0;  // unknown until E
      p.split_position = (fu_header & 0x80) ? 0 : 1;
      p.flags = 0;
      if (fu_header & 0x80) p.flags |= kFlagAuStart;
      if (fu_header & 0x40) p.flags |= kFlagAuEnd;
      p.keyframe = (orig_type == 5);
    } else {
      if (err) *err = "rtp: unsupported NAL type " + std::to_string(nal_type);
      return false;
    }
    *out = p;
    return true;
  }
  return wire::parse_legacy_header(packet, len, out, err);
}

}  // namespace proto
}  // namespace usbdisplay
