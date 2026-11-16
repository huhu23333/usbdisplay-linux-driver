// SPDX-License-Identifier: MIT
// 离线转储文件：用于在不访问硬件的情况下校验报文封装。
// 首部为八字节魔数 "USBDRPT1"，随后四字节小端序报告长度。
// 每条记录先写四字节小端序记录长度，再写该长度的原始报告内容。
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace usbdisplay {
namespace hid {

struct ReportDumpRecord {
  std::vector<uint8_t> report;  // full report bytes including report-id byte
};

class ReportDumpWriter {
 public:
  ~ReportDumpWriter();
  bool open(const std::string& path, size_t report_size, std::string* err);
  bool write(const uint8_t* report, size_t len, std::string* err);
  void close();
  bool is_open() const { return file_ != nullptr; }
  size_t records_written() const { return records_; }

 private:
  void* file_ = nullptr;
  uint32_t report_size_ = 0;
  size_t records_ = 0;
};

bool read_report_dump(const std::string& path,
                      std::vector<ReportDumpRecord>* out, uint32_t* report_size,
                      std::string* err);

}  // namespace hid
}  // namespace usbdisplay
