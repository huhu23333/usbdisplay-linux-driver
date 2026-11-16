// SPDX-License-Identifier: MIT
#include <cstring>
#include <vector>

#include "common/util.h"
#include "hid/hid_profile.h"
#include "hid/report_dump.h"
#include "hid/report_sink.h"
#include "test_harness.h"
#include "transport/hidraw.h"

int main() {
  using namespace usbdisplay;
  {
    // 外层头部：报告编号、大端序负载长度、参数三、参数四与三个零字节。
    uint8_t header[hid::kOuterHeaderSize] = {0};
    hid::write_outer_header(header, 1, 100, 0x11223344, 0x55667788);
    CHECK(header[0] == 1);
    CHECK(read_be32(header + 1) == 100);
    CHECK(read_be32(header + 5) == 0x11223344);
    CHECK(read_be32(header + 9) == 0x55667788);
    CHECK(header[13] == 0 && header[14] == 0 && header[15] == 0);
    hid::OuterHeader parsed;
    std::string err;
    CHECK(hid::parse_outer_header(header, sizeof(header), &parsed, &err));
    CHECK(parsed.report_id == 1 && parsed.length == 100);
    CHECK(parsed.param3 == 0x11223344 && parsed.param4 == 0x55667788);
    CHECK(!hid::parse_outer_header(header, 8, &parsed, &err));
  }
  {
    // 一万字节消息拆分为 4096 字节报告，每个报告携带自己的头部与负载分片。
    std::vector<uint8_t> payload(10000);
    for (size_t i = 0; i < payload.size(); ++i) {
      payload[i] = static_cast<uint8_t>(i * 7 + 3);
    }
    std::vector<std::vector<uint8_t>> reports;
    std::string err;
    CHECK(hid::build_reports(payload.data(), payload.size(), 1, 4096, true, 0,
                             0, &reports, &err));
    CHECK(reports.size() == 3);
    CHECK(reports[0].size() == 4096);
    CHECK(reports[0][0] == 1);
    // 每个报告都写入整条消息的总负载长度。
    CHECK(read_be32(reports[0].data() + 1) == 10000);
    CHECK(read_be32(reports[1].data() + 1) == 10000);
    CHECK(read_be32(reports[2].data() + 1) == 10000);
    std::vector<uint8_t> reassembled;
    size_t remaining = payload.size();
    for (const auto& report : reports) {
      const size_t take = std::min<size_t>(4080, remaining);
      reassembled.insert(reassembled.end(), report.begin() + 16,
                         report.begin() + 16 + take);
      remaining -= take;
    }
    CHECK(reassembled == payload);
  }
  {
    // 参数三与参数四只写入首个报告。
    std::vector<uint8_t> payload(9000, 0x33);
    std::vector<std::vector<uint8_t>> reports;
    std::string err;
    CHECK(hid::build_reports(payload.data(), payload.size(), 2, 12288, true,
                             0xAABBCCDD, 0x11223344, &reports, &err));
    CHECK(reports.size() == 1);
    CHECK(read_be32(reports[0].data() + 5) == 0xAABBCCDD);
    CHECK(read_be32(reports[0].data() + 9) == 0x11223344);
    std::vector<uint8_t> big(20000, 0x44);
    reports.clear();
    CHECK(hid::build_reports(big.data(), big.size(), 2, 12288, true,
                             0xAABBCCDD, 0x11223344, &reports, &err));
    CHECK(reports.size() == 2);
    CHECK(read_be32(reports[0].data() + 5) == 0xAABBCCDD);
    CHECK(read_be32(reports[1].data() + 5) == 0);
    CHECK(read_be32(reports[1].data() + 9) == 0);
    CHECK(read_be32(reports[1].data() + 1) == 20000);
  }
  {
    // 较小的分片长度。
    std::vector<uint8_t> payload(2500, 0x42);
    std::vector<std::vector<uint8_t>> reports;
    std::string err;
    CHECK(hid::build_reports(payload.data(), payload.size(), 2, 12288, true, 0,
                             0, &reports, &err, 1000));
    CHECK(reports.size() == 3);
    CHECK(read_be32(reports[0].data() + 1) == 2500);
    CHECK(read_be32(reports[1].data() + 1) == 2500);
    CHECK(read_be32(reports[2].data() + 1) == 2500);
  }
  {
    // Ugreen 外层格式使用 14 字节小端序头部。
    std::vector<uint8_t> payload(5000, 0x5A);
    std::vector<std::vector<uint8_t>> reports;
    std::string err;
    CHECK(hid::build_reports(payload.data(), payload.size(), 2, 4096, true, 9,
                             0x55, &reports, &err, 0,
                             hid::OuterFraming::Ugreen14, 7, 0x1234));
    CHECK(reports.size() == 2);
    CHECK(reports[0][0] == 2);
    CHECK(reports[0][1] == 7);
    CHECK(read_le16(reports[0].data() + 2) == 0x1234);
    CHECK(read_le32(reports[0].data() + 4) == 9);     // seq
    CHECK(read_le32(reports[0].data() + 8) == 0x55);  // fraginfo
    CHECK(read_le16(reports[0].data() + 12) == 4096 - 14);
    CHECK(reports[0][14] == 0x5A);
    CHECK(read_le16(reports[1].data() + 12) == 5000 - (4096 - 14));
  }
  {
    // 32 字节视频内层头部与负载。
    const uint8_t annexb[] = {0, 0, 0, 1, 0x65, 0x88, 0xAA};
    const auto message = hid::build_video_message(annexb, sizeof(annexb), 7,
                                                  0x1234, 1, 2, 3, 4, true);
    CHECK(message.size() == hid::kInnerHeaderSize + sizeof(annexb));
    CHECK(read_le32(message.data() + 0) == 7);
    CHECK(read_le32(message.data() + 4) == sizeof(annexb) + 24);
    CHECK(read_le32(message.data() + 8) == 0x1234);
    CHECK(read_le16(message.data() + 12) == 0x1000);
    CHECK(read_le32(message.data() + 16) == 1);
    CHECK(read_le32(message.data() + 20) == 2);
    CHECK(read_le32(message.data() + 24) == 3);
    CHECK(read_le32(message.data() + 28) == 4);
    CHECK(std::memcmp(message.data() + hid::kInnerHeaderSize, annexb,
                      sizeof(annexb)) == 0);
    // 36 字节内层头部变体，末尾附带负载长度。
    const auto message36 = hid::build_video_message(
        annexb, sizeof(annexb), 7, 0x1234, 1, 2, 3, 4, true, true);
    CHECK(message36.size() == 36 + sizeof(annexb));
    CHECK(read_le32(message36.data() + 32) == sizeof(annexb));
    CHECK(std::memcmp(message36.data() + 36, annexb, sizeof(annexb)) == 0);
    hid::InnerHeader inner;
    std::string err;
    CHECK(hid::parse_inner_header(message.data(), message.size(), &inner, &err));
    CHECK(inner.seq == 7 && inner.msg_type == 0x1234 && inner.magic == 0x1000);
    CHECK(!hid::parse_inner_header(message.data(), 4, &inner, &err));
  }
  {
    // Ugreen 内层头部。
    const uint8_t annexb[] = {0, 0, 0, 1, 0x65};
    const auto message = hid::build_ugreen_video_message(
        annexb, sizeof(annexb), 11, 3, "TADV", 5, 6, 7);
    CHECK(message.size() == hid::kInnerHeaderSize + sizeof(annexb));
    CHECK(std::memcmp(message.data(), "TADV", 4) == 0);
    CHECK(read_le32(message.data() + 4) == sizeof(annexb) + 24);
    CHECK(read_le32(message.data() + 8) == 3);
    CHECK(read_le32(message.data() + 12) == 11);
    CHECK(read_le32(message.data() + 16) == 5);
    CHECK(read_le32(message.data() + 20) == 6);
    CHECK(read_le32(message.data() + 24) == 7);
    CHECK(std::memcmp(message.data() + 32, annexb, sizeof(annexb)) == 0);
  }
  {
    // 转储文件读写往返。
    const std::string path = "/tmp/usbdisplay_test_hid_dump.bin";
    hid::ReportDumpWriter writer;
    std::string err;
    CHECK(writer.open(path, 4096, &err));
    std::vector<uint8_t> report(4096, 0x11);
    report[0] = 1;
    CHECK(writer.write(report.data(), report.size(), &err));
    report[0] = 2;
    CHECK(writer.write(report.data(), report.size(), &err));
    writer.close();
    std::vector<hid::ReportDumpRecord> records;
    uint32_t report_size = 0;
    CHECK(hid::read_report_dump(path, &records, &report_size, &err));
    CHECK(report_size == 4096);
    CHECK(records.size() == 2);
    CHECK(records[0].report.size() == 4096);
    CHECK(records[0].report[0] == 1);
    CHECK(records[1].report[0] == 2);
    ::remove(path.c_str());
  }
  {
    // IF0 报告描述符：报告编号 1 的数据长度为 4095 字节。
    const std::vector<uint8_t> if0 = {
        0x06, 0xFF, 0xFF, 0x09, 0x00, 0xA1, 0x01, 0x85, 0x01, 0x15, 0x00, 0x25,
        0xFF, 0x19, 0x00, 0x29, 0xFF, 0x75, 0x08, 0x96, 0xFF, 0x0F, 0x81, 0x02,
        0x19, 0x00, 0x29, 0xFF, 0x96, 0xFF, 0x0F, 0x91, 0x02, 0xC0, 0x05, 0x0D,
        0x09, 0x0E, 0xA1, 0x01, 0x85, 0x20, 0x09, 0x23, 0xA1, 0x02, 0x09, 0x52,
        0x09, 0x53, 0x15, 0x00, 0x25, 0x10, 0x75, 0x08, 0x95, 0x02, 0xB1, 0x02,
        0xC0, 0xC0};
    usbdisplay::transport::HidReportSizes sizes;
    std::string err;
    CHECK(usbdisplay::transport::parse_hid_report_descriptor(if0, &sizes, &err));
    CHECK(sizes.input.size() == 1);
    CHECK(sizes.output.size() == 1);
    CHECK(sizes.input[1] == 4095);
    CHECK(sizes.output[1] == 4095);
    CHECK(sizes.feature[0x20] == 2);
    CHECK(sizes.output[1] + 1 == 4096);

    // IF1 报告描述符：报告编号 2 的数据长度为 12287 字节。
    const std::vector<uint8_t> if1 = {
        0x06, 0xFF, 0xFF, 0x09, 0x00, 0xA1, 0x01, 0x85, 0x02, 0x15, 0x00, 0x25,
        0xFF, 0x19, 0x00, 0x29, 0xFF, 0x75, 0x08, 0x96, 0xFF, 0x2F, 0x81, 0x02,
        0x19, 0x00, 0x29, 0xFF, 0x96, 0xFF, 0x2F, 0x91, 0x02, 0xC0, 0x05, 0x0D,
        0x09, 0x0E, 0xA1, 0x01, 0x85, 0x20, 0x09, 0x23, 0xA1, 0x02, 0x09, 0x52,
        0x09, 0x53, 0x15, 0x00, 0x25, 0x10, 0x75, 0x08, 0x95, 0x02, 0xB1, 0x02,
        0xC0, 0xC0};
    CHECK(usbdisplay::transport::parse_hid_report_descriptor(if1, &sizes, &err));
    CHECK(sizes.input.size() == 1);
    CHECK(sizes.output.size() == 1);
    CHECK(sizes.input[2] == 12287);
    CHECK(sizes.output[2] == 12287);
    CHECK(sizes.output[2] + 1 == 12288);
  }
  return testharness::finish("test_hid");
}
