// SPDX-License-Identifier: MIT
#include <cstring>
#include <vector>

#include "common/util.h"
#include "proto/wire.h"
#include "test_harness.h"

int main() {
  using namespace usbdisplay;
  using namespace usbdisplay::proto;
  {
    // legacy 头部固定字节：视频、关键帧与访问单元结束、流编号零、序号 42、帧号 7。
    uint8_t header[kLegacyHeaderSize] = {0};
    wire::write_legacy_header(header, MediaKind::Video,
                              static_cast<uint8_t>(kFlagKeyframe | kFlagAuEnd),
                              0, 42, 7, 3, 2, 4, 0x0102030405060708ULL);
    const uint8_t expected[kLegacyHeaderSize] = {
        0x45, 0x5A, 0x44, 0x50,  // "EZDP"
        0x01,                    // version
        0x01,                    // kind video
        0x05,                    // keyframe | au_end
        0x00,                    // stream 0
        0x2A, 0x00, 0x00, 0x00,  // seq 42
        0x07, 0x00, 0x00, 0x00,  // frame 7
        0x03, 0x00,              // split count 3
        0x02, 0x00,              // split position 2
        0x04, 0x00, 0x00, 0x00,  // au size 4
        0x08, 0x07, 0x06, 0x05, 0x04, 0x03, 0x02, 0x01,  // pts LE
    };
    CHECK(std::memcmp(header, expected, kLegacyHeaderSize) == 0);

    ParsedPacket parsed;
    std::string err;
    uint8_t packet[kLegacyHeaderSize + 4];
    std::memcpy(packet, header, kLegacyHeaderSize);
    std::memcpy(packet + kLegacyHeaderSize, "ABCD", 4);
    CHECK(wire::parse_legacy_header(packet, sizeof(packet), &parsed, &err));
    CHECK(parsed.kind == MediaKind::Video);
    CHECK(parsed.keyframe);
    CHECK(parsed.seq == 42 && parsed.frame_id == 7);
    CHECK(parsed.split_count == 3 && parsed.split_position == 2);
    CHECK(parsed.au_size == 4 && parsed.pts_us == 0x0102030405060708ULL);
    CHECK(parsed.payload_len == 4);
    CHECK(std::memcmp(parsed.payload, "ABCD", 4) == 0);

    header[0] = 0;  // break magic
    CHECK(!wire::parse_legacy_header(header, sizeof(header), &parsed, &err));
    CHECK(!err.empty());
    CHECK(!wire::parse_legacy_header(header, 8, &parsed, &err));
  }
  {
    uint8_t rtp[12] = {0};
    wire::write_rtp_header(rtp, true, 96, 0x1234, 0xDEADBEEF, 0x01020304);
    CHECK(rtp[0] == 0x80);
    CHECK(rtp[1] == 0xE0);  // marker + PT 96
    CHECK(read_be16(rtp + 2) == 0x1234);
    CHECK(read_be32(rtp + 4) == 0xDEADBEEF);
    CHECK(read_be32(rtp + 8) == 0x01020304);
    uint8_t packet[16];
    std::memcpy(packet, rtp, 12);
    std::memset(packet + 12, 0xAA, 4);
    ParsedPacket parsed;
    std::string err;
    CHECK(wire::parse_rtp_header(packet, sizeof(packet), &parsed, &err));
    CHECK(parsed.marker && parsed.rtp_seq == 0x1234);
    CHECK(parsed.rtp_timestamp == 0xDEADBEEF);
    CHECK(parsed.pts_us == static_cast<uint64_t>(0xDEADBEEF) * 1000000ULL / 90000ULL);
    CHECK(parsed.payload_len == 4);
    CHECK(!wire::parse_rtp_header(packet, 8, &parsed, &err));
  }
  {
    const uint8_t framed[] = {0x00, 0x00, 0x00, 0x03, 0xAA, 0xBB, 0xCC};
    size_t consumed = 0;
    const uint8_t* payload = nullptr;
    size_t payload_len = 0;
    CHECK(Packetizer::strip_length_prefix(framed, sizeof(framed), &consumed,
                                          &payload, &payload_len));
    CHECK(consumed == sizeof(framed));
    CHECK(payload_len == 3 && payload[0] == 0xAA);
    CHECK(!Packetizer::strip_length_prefix(framed, 4, &consumed, &payload,
                                          &payload_len));
    std::vector<uint8_t> pkt = {1, 2, 3};
    Packetizer::append_length_prefix(pkt);
    CHECK(pkt.size() == 7 && pkt[3] == 3 && pkt[4] == 1);
  }
  return testharness::finish("test_wire");
}
