// SPDX-License-Identifier: MIT
// Offline HID report dump: lets us validate the exact report framing without
// touching hardware (usbdisplay_send --dry-run --dump-reports FILE, then
// fake_device --hid-dump FILE).
//
// File layout:
//   magic  "USBDRPT1" (8 bytes)
//   u32 LE report_size (total bytes per report including the report-id byte)
//   records: u32 LE length, then `length` raw report bytes
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
