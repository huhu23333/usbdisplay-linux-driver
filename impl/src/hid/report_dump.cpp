// SPDX-License-Identifier: MIT
#include "hid/report_dump.h"

#include <cstdio>
#include <cstring>

#include "common/util.h"

namespace usbdisplay {
namespace hid {
namespace {
const char kMagic[8] = {'U', 'S', 'B', 'D', 'R', 'P', 'T', '1'};
}  // namespace

ReportDumpWriter::~ReportDumpWriter() { close(); }

bool ReportDumpWriter::open(const std::string& path, size_t report_size,
                            std::string* err) {
  close();
  if (report_size == 0 || report_size > 0xFFFFFFFFu) {
    if (err) *err = "invalid report size for dump";
    return false;
  }
  std::FILE* f = std::fopen(path.c_str(), "wb");
  if (f == nullptr) {
    if (err) *err = "cannot create report dump: " + path;
    return false;
  }
  uint8_t header[12] = {0};
  std::memcpy(header, kMagic, 8);
  write_le32(header + 8, static_cast<uint32_t>(report_size));
  if (std::fwrite(header, 1, sizeof(header), f) != sizeof(header)) {
    std::fclose(f);
    if (err) *err = "report dump header write failed";
    return false;
  }
  file_ = f;
  report_size_ = static_cast<uint32_t>(report_size);
  records_ = 0;
  return true;
}

bool ReportDumpWriter::write(const uint8_t* report, size_t len,
                             std::string* err) {
  if (file_ == nullptr) {
    if (err) *err = "report dump not open";
    return false;
  }
  if (len != report_size_) {
    if (err) {
      *err = "report size mismatch: got " + std::to_string(len) +
             " expected " + std::to_string(report_size_);
    }
    return false;
  }
  uint8_t length_bytes[4];
  write_le32(length_bytes, static_cast<uint32_t>(len));
  if (std::fwrite(length_bytes, 1, 4, static_cast<std::FILE*>(file_)) != 4 ||
      std::fwrite(report, 1, len, static_cast<std::FILE*>(file_)) != len) {
    if (err) *err = "report dump write failed";
    return false;
  }
  ++records_;
  return true;
}

void ReportDumpWriter::close() {
  if (file_ != nullptr) {
    std::fclose(static_cast<std::FILE*>(file_));
    file_ = nullptr;
  }
}

bool read_report_dump(const std::string& path,
                      std::vector<ReportDumpRecord>* out, uint32_t* report_size,
                      std::string* err) {
  out->clear();
  std::vector<uint8_t> bytes;
  if (!read_file(path, &bytes, err)) return false;
  if (bytes.size() < 12 || std::memcmp(bytes.data(), kMagic, 8) != 0) {
    if (err) *err = "not a USBDRPT1 report dump";
    return false;
  }
  const uint32_t size = read_le32(bytes.data() + 8);
  if (report_size) *report_size = size;
  size_t pos = 12;
  while (pos + 4 <= bytes.size()) {
    const uint32_t len = read_le32(bytes.data() + pos);
    pos += 4;
    if (len == 0 || len > 1u * 1024 * 1024 || pos + len > bytes.size()) {
      if (err) *err = "corrupt report dump record";
      return false;
    }
    ReportDumpRecord record;
    record.report.assign(bytes.begin() + static_cast<long>(pos),
                         bytes.begin() + static_cast<long>(pos + len));
    out->push_back(std::move(record));
    pos += len;
  }
  if (pos != bytes.size()) {
    if (err) *err = "trailing bytes in report dump";
    return false;
  }
  return true;
}

}  // namespace hid
}  // namespace usbdisplay
