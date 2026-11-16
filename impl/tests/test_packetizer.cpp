// SPDX-License-Identifier: MIT
#include <cstring>
#include <vector>

#include "common/util.h"
#include "test_harness.h"
#include "usbdisplay/protocol.h"

using namespace usbdisplay;
using namespace usbdisplay::proto;

int main() {
  {
    // legacy 格式拆分与固定头部字节。
    PacketizeOptions options;
    options.max_payload = 1;
    Packetizer packetizer(options);
    const uint8_t payload[] = {'A', 'B', 'C', 'D'};
    std::vector<std::vector<uint8_t>> packets;
    packetizer.packetize(payload, sizeof(payload), MediaKind::Video,
                         0x0102030405060708ULL, true, 7, packets);
    CHECK(packets.size() == 4);
    CHECK(packetizer.next_seq() == 4);
    {
      const auto& p = packets[0];
      CHECK(p.size() == kLegacyHeaderSize + 1);
      CHECK(p[0] == 0x45 && p[1] == 0x5A && p[2] == 0x44 && p[3] == 0x50);
      CHECK(p[4] == 1 && p[5] == 1);
      CHECK(p[6] == static_cast<uint8_t>(kFlagKeyframe | kFlagAuStart));
      CHECK(read_le16(p.data() + 16) == 4);   // split count
      CHECK(read_le16(p.data() + 18) == 0);   // split position
      CHECK(read_le32(p.data() + 20) == 4);   // au size
      CHECK(read_le64(p.data() + 24) == 0x0102030405060708ULL);
      CHECK(p[kLegacyHeaderSize] == 'A');
    }
    {
      const auto& p = packets[2];
      CHECK(p[6] == static_cast<uint8_t>(kFlagKeyframe));
      CHECK(read_le32(p.data() + 8) == 2);  // seq
      CHECK(read_le16(p.data() + 18) == 2);  // split position
      CHECK(p[kLegacyHeaderSize] == 'C');
    }
    {
      const auto& p = packets[3];
      CHECK(p[6] == static_cast<uint8_t>(kFlagKeyframe | kFlagAuEnd));
      CHECK(p[kLegacyHeaderSize] == 'D');
    }

    Depacketizer depacketizer;
    std::vector<uint8_t> reassembled;
    for (const auto& p : packets) {
      ParsedPacket parsed;
      std::string err;
      CHECK(depacketizer.parse(p.data(), p.size(), &parsed, &err));
      CHECK(parsed.split_count == 4);
      CHECK(parsed.pts_us == 0x0102030405060708ULL);
      reassembled.insert(reassembled.end(), parsed.payload,
                         parsed.payload + parsed.payload_len);
    }
    CHECK(reassembled.size() == 4 &&
          std::memcmp(reassembled.data(), payload, 4) == 0);

    // 第二个访问单元延续报文序号。
    std::vector<std::vector<uint8_t>> more;
    packetizer.packetize(payload, sizeof(payload), MediaKind::Video, 100, false,
                         8, more);
    CHECK(read_le32(more[0].data() + 8) == 4);
  }
  {
    // TCP 长度前缀。
    PacketizeOptions options;
    options.length_prefix = true;
    options.max_payload = 2;
    Packetizer packetizer(options);
    const uint8_t payload[] = {1, 2, 3};
    std::vector<std::vector<uint8_t>> packets;
    packetizer.packetize(payload, sizeof(payload), MediaKind::Audio, 0, false,
                         0, packets);
    CHECK(packets.size() == 2);
    const uint8_t* inner = nullptr;
    size_t inner_len = 0;
    size_t consumed = 0;
    CHECK(Packetizer::strip_length_prefix(packets[0].data(), packets[0].size(),
                                          &consumed, &inner, &inner_len));
    CHECK(consumed == packets[0].size());
    CHECK(inner_len == packets[0].size() - 4);
    ParsedPacket parsed;
    std::string err;
    CHECK(Depacketizer().parse(inner, inner_len, &parsed, &err));
    CHECK(parsed.kind == MediaKind::Audio && parsed.payload_len == 2);
  }
  {
    // legacy 流配置报文。
    PacketizeOptions options;
    Packetizer packetizer(options);
    std::vector<std::vector<uint8_t>> packets;
    packetizer.packetize_config("{\"type\":\"stream-config\"}", 0, packets);
    CHECK(packets.size() == 1);
    ParsedPacket parsed;
    std::string err;
    CHECK(Depacketizer().parse(packets[0].data(), packets[0].size(), &parsed,
                               &err));
    CHECK(parsed.kind == MediaKind::Config);
    CHECK(parsed.flags & kFlagConfig);
    CHECK(std::string(reinterpret_cast<const char*>(parsed.payload),
                      parsed.payload_len) == "{\"type\":\"stream-config\"}");
  }
  {
    // RTP 对较大的 IDR NAL 单元进行 FU-A 分片。
    std::vector<uint8_t> au;
    au.push_back(0);
    au.push_back(0);
    au.push_back(0);
    au.push_back(1);
    au.push_back(0x65);  // IDR, nri=3
    for (int i = 0; i < 240; ++i) au.push_back(0xAA);
    PacketizeOptions options;
    options.format = WireFormat::RtpAvc;
    options.max_payload = 100;
    Packetizer packetizer(options);
    std::vector<std::vector<uint8_t>> packets;
    packetizer.packetize(au.data(), au.size(), MediaKind::Video, 90000, true, 0,
                         packets);
    CHECK(packets.size() > 1);
    for (const auto& p : packets) CHECK(p.size() <= 100);
    CHECK(packets[0][1] == 96);                    // payload type
    CHECK((packets[0][1] & 0x80) == 0);            // marker off
    CHECK((packets.back()[1] & 0x80) != 0);        // marker on
    CHECK((packets[0][12] & 0x1f) == 28);          // FU-A indicator
    CHECK((packets[0][13] & 0x80) != 0);           // start bit
    CHECK((packets[0][13] & 0x1f) == 5);           // original type IDR
    CHECK((packets.back()[13] & 0x40) != 0);       // end bit
    const uint32_t expected_ssrc = options.ssrc;
    CHECK(read_be32(packets[0].data() + 8) == expected_ssrc);
    CHECK(read_be32(packets[0].data() + 4) == 90000u * 9u / 100u);

    Depacketizer depacketizer(WireFormat::RtpAvc);
    size_t fragments = 0;
    for (size_t i = 0; i < packets.size(); ++i) {
      ParsedPacket parsed;
      std::string err;
      CHECK(depacketizer.parse(packets[i].data(), packets[i].size(), &parsed,
                               &err));
      CHECK(parsed.rtp_seq == static_cast<uint16_t>(i));
      CHECK(parsed.keyframe);
      if (i == 0) CHECK(parsed.flags & kFlagAuStart);
      if (i + 1 == packets.size()) CHECK(parsed.flags & kFlagAuEnd);
      fragments += parsed.payload_len;
    }
    // FU-A 负载只包含 NAL 主体内容。
    CHECK(fragments == au.size() - 5);
  }
  {
    // 较小的 NAL 单元使用单 NAL 打包。
    std::vector<uint8_t> au = {0, 0, 1, 0x41, 0x88, 0x01, 0x02};
    PacketizeOptions options;
    options.format = WireFormat::RtpAvc;
    options.max_payload = 1400;
    Packetizer packetizer(options);
    std::vector<std::vector<uint8_t>> packets;
    packetizer.packetize(au.data(), au.size(), MediaKind::Video, 0, false, 0,
                         packets);
    CHECK(packets.size() == 1);
    CHECK(packets[0].size() == 12 + 4);
    ParsedPacket parsed;
    std::string err;
    CHECK(Depacketizer(WireFormat::RtpAvc)
              .parse(packets[0].data(), packets[0].size(), &parsed, &err));
    CHECK(parsed.flags ==
          static_cast<uint8_t>(kFlagAuStart | kFlagAuEnd));
    CHECK(!parsed.keyframe);
    CHECK(parsed.payload_len == 4 && parsed.payload[0] == 0x41);
  }
  {
    // 格式错误的报文应被拒绝。
    uint8_t garbage[40] = {0};
    ParsedPacket parsed;
    std::string err;
    CHECK(!Depacketizer().parse(garbage, sizeof(garbage), &parsed, &err));
    CHECK(!Depacketizer(WireFormat::RtpAvc)
               .parse(garbage, 4, &parsed, &err));
  }
  return testharness::finish("test_packetizer");
}
