// SPDX-License-Identifier: MIT
#include "hid/report_sink.h"

#include <algorithm>
#include <cstring>
#include <vector>

#include "common/log.h"
#include "common/util.h"
#include "hid/hid_profile.h"
#include "hid/report_dump.h"
#include "transport/hidraw.h"

namespace usbdisplay {
namespace hid {
namespace {
constexpr size_t kMaxMessage = 64u * 1024 * 1024;
}

bool build_reports(const uint8_t* payload, size_t len, uint8_t report_id,
                   size_t report_size, bool header_per_chunk,
                   uint32_t param3, uint32_t param4,
                   std::vector<std::vector<uint8_t>>* out, std::string* err,
                   size_t chunk_budget, OuterFraming framing,
                   uint8_t ugreen_type, uint16_t ugreen_flags,
                   bool params_every_report) {
  out->clear();
  // The ugreen "mode B" outer header is 14 bytes little-endian:
  //   [0]=report id, [1]=type, [2..4)=flags u16,
  //   [4..8)=seq u32, [8..12)=fraginfo u32, [12..14)=chunk length u16.
  const size_t header_size =
      framing == OuterFraming::App16 ? kOuterHeaderSize : 14;
  if (report_size <= header_size) {
    if (err) *err = "report size too small for the outer header";
    return false;
  }
  if (len > kMaxMessage) {
    if (err) *err = "message too large";
    return false;
  }
  (void)header_per_chunk;  // verified layout: every report carries a header
  size_t budget = report_size - header_size;
  if (chunk_budget != 0) budget = std::min(budget, chunk_budget);
  if (budget == 0) {
    if (err) *err = "zero payload chunk budget";
    return false;
  }
  size_t offset = 0;
  bool first = true;
  for (;;) {
    const size_t chunk = std::min(budget, len - offset);
    std::vector<uint8_t> report(report_size, 0);
    if (framing == OuterFraming::App16) {
      // Verified app framing: every report repeats the TOTAL payload length;
      // param3/param4 appear in the first report only (SendHIDVideo).
      const uint32_t p3 = (first || params_every_report) ? param3 : 0;
      const uint32_t p4 = (first || params_every_report) ? param4 : 0;
      write_outer_header(report.data(), report_id, len, p3, p4);
    } else {
      report[0] = report_id;
      report[1] = ugreen_type;
      write_le16(report.data() + 2, ugreen_flags);
      write_le32(report.data() + 4, param3);  // seq
      write_le32(report.data() + 8, param4);  // fraginfo
      write_le16(report.data() + 12, static_cast<uint16_t>(chunk));
    }
    if (chunk > 0) {
      std::memcpy(report.data() + header_size, payload + offset, chunk);
    }
    offset += chunk;
    first = false;
    out->push_back(std::move(report));
    if (offset >= len) break;
  }
  return true;
}

namespace {

class HidrawReportSink : public ReportSink {
 public:
  HidrawReportSink(std::string path, uint8_t report_id, size_t report_size,
                   bool header_per_chunk, size_t chunk_budget)
      : path_(std::move(path)),
        report_id_(report_id),
        report_size_(report_size),
        header_per_chunk_(header_per_chunk),
        chunk_budget_(chunk_budget) {}

  bool open(std::string* err) override {
    if (!device_.open(path_, err)) return false;
    transport::HidrawInfo info;
    device_.get_info(&info, nullptr);
    log::info("hid report sink " + path_ + ": " + info.name + " vid/pid " +
              std::to_string(info.vid) + "/" + std::to_string(info.pid));
    return true;
  }
  void close() override { device_.close(); }
  bool connected() const override { return device_.is_open(); }

  bool send_message(const uint8_t* payload, size_t len, uint32_t param3,
                    uint32_t param4, std::string* err) override {
    std::vector<std::vector<uint8_t>> reports;
    if (!build_reports(payload, len, report_id_, report_size_,
                       header_per_chunk_, param3, param4, &reports, err,
                       chunk_budget_, outer_framing(), ugreen_type_,
                       ugreen_flags_, params_every_report_)) {
      return false;
    }
    for (const auto& report : reports) {
      if (!device_.write_raw(report.data(), report.size(), err)) return false;
      ++reports_sent_;
      if (log::enabled(TraceLevel::Wire)) {
        log::wire("hid tx " + std::to_string(report.size()) + "B " +
                  hexdump(report.data(), report.size(), 64));
      }
    }
    ++messages_sent_;
    return true;
  }

  bool recv_message(std::string* payload, int timeout_ms, bool* timed_out,
                    std::string* err) override {
    if (timed_out) *timed_out = false;
    std::vector<uint8_t> buffer(report_size_ + 1, 0);
    const int n = device_.read_report(buffer.data(), buffer.size(), timeout_ms,
                                      err);
    if (n < 0) return false;
    if (n == 0) {
      if (timed_out) *timed_out = true;
      return false;
    }
    const size_t raw_len = static_cast<size_t>(n);
    // Some firmware builds answer with plain JSON instead of the framed reply.
    const size_t start = (raw_len > 0 && buffer[0] == report_id_) ? 1 : 0;
    if (raw_len > start && (buffer[start] == '{' || buffer[start] == '[')) {
      size_t end = raw_len;
      while (end > start && buffer[end - 1] == 0) --end;
      payload->assign(reinterpret_cast<const char*>(buffer.data() + start),
                      end - start);
      return true;
    }
    if (raw_len < kOuterHeaderSize) {
      payload->assign(reinterpret_cast<const char*>(buffer.data()), raw_len);
      return true;
    }
    OuterHeader header;
    std::string herr;
    if (!parse_outer_header(buffer.data(), raw_len, &header, &herr)) {
      size_t end = raw_len;
      while (end > 0 && buffer[end - 1] == 0) --end;
      payload->assign(reinterpret_cast<const char*>(buffer.data()), end);
      return true;
    }
    size_t take = header.length;
    if (take > raw_len - kOuterHeaderSize) take = raw_len - kOuterHeaderSize;
    std::string out(reinterpret_cast<const char*>(buffer.data() + kOuterHeaderSize),
                    take);
    int guard = 0;
    while (!out.empty() && (out[0] == '{' || out[0] == '[') &&
           std::count(out.begin(), out.end(), '{') >
               std::count(out.begin(), out.end(), '}') &&
           guard < 64) {
      std::vector<uint8_t> next(report_size_ + 1, 0);
      std::string next_err;
      const int m = device_.read_report(next.data(), next.size(), timeout_ms,
                                        &next_err);
      if (m < static_cast<int>(kOuterHeaderSize)) break;
      OuterHeader next_header;
      if (!parse_outer_header(next.data(), static_cast<size_t>(m),
                              &next_header, &next_err)) {
        break;
      }
      size_t next_take = next_header.length;
      if (next_take > static_cast<size_t>(m) - kOuterHeaderSize) {
        next_take = static_cast<size_t>(m) - kOuterHeaderSize;
      }
      out.append(reinterpret_cast<const char*>(next.data() + kOuterHeaderSize),
                 next_take);
      ++guard;
    }
    *payload = std::move(out);
    return true;
  }

  std::string describe() const override {
    return "hidraw:" + path_ + " (report " + std::to_string(report_id_) + "/" +
           std::to_string(report_size_) + "B)";
  }
  uint8_t report_id() const override { return report_id_; }
  size_t report_size() const override { return report_size_; }
  size_t payload_budget() const override {
    return report_size_ > kOuterHeaderSize ? report_size_ - kOuterHeaderSize : 0;
  }
  size_t reports_sent() const override { return reports_sent_; }
  size_t messages_sent() const override { return messages_sent_; }

 private:
  std::string path_;
  uint8_t report_id_;
  size_t report_size_;
  bool header_per_chunk_;
  transport::HidrawDevice device_;
  size_t reports_sent_ = 0;
  size_t messages_sent_ = 0;
  size_t chunk_budget_ = 0;
};

class RecordingReportSink : public ReportSink {
 public:
  RecordingReportSink(std::string path, uint8_t report_id, size_t report_size,
                      bool header_per_chunk, size_t chunk_budget)
      : path_(std::move(path)),
        report_id_(report_id),
        report_size_(report_size),
        header_per_chunk_(header_per_chunk),
        chunk_budget_(chunk_budget) {}

  bool open(std::string* err) override {
    return writer_.open(path_, report_size_, err);
  }
  void close() override { writer_.close(); }
  bool connected() const override { return writer_.is_open(); }

  bool send_message(const uint8_t* payload, size_t len, uint32_t param3,
                    uint32_t param4, std::string* err) override {
    std::vector<std::vector<uint8_t>> reports;
    if (!build_reports(payload, len, report_id_, report_size_,
                       header_per_chunk_, param3, param4, &reports, err,
                       chunk_budget_, outer_framing(), ugreen_type_,
                       ugreen_flags_, params_every_report_)) {
      return false;
    }
    for (const auto& report : reports) {
      if (!writer_.write(report.data(), report.size(), err)) return false;
      ++reports_sent_;
    }
    ++messages_sent_;
    return true;
  }

  bool recv_message(std::string*, int, bool* timed_out,
                    std::string* err) override {
    if (timed_out) *timed_out = true;
    if (err) *err = "recording sink does not receive";
    return false;
  }

  std::string describe() const override { return "dump:" + path_; }
  uint8_t report_id() const override { return report_id_; }
  size_t report_size() const override { return report_size_; }
  size_t payload_budget() const override {
    return report_size_ > kOuterHeaderSize ? report_size_ - kOuterHeaderSize : 0;
  }
  size_t reports_sent() const override { return reports_sent_; }
  size_t messages_sent() const override { return messages_sent_; }

 private:
  std::string path_;
  uint8_t report_id_;
  size_t report_size_;
  bool header_per_chunk_;
  ReportDumpWriter writer_;
  size_t reports_sent_ = 0;
  size_t messages_sent_ = 0;
  size_t chunk_budget_ = 0;
};

class NullReportSink : public ReportSink {
 public:
  NullReportSink(uint8_t report_id, size_t report_size, size_t chunk_budget)
      : report_id_(report_id),
        report_size_(report_size),
        chunk_budget_(chunk_budget) {}
  bool open(std::string*) override {
    open_ = true;
    return true;
  }
  void close() override { open_ = false; }
  bool connected() const override { return open_; }
  bool send_message(const uint8_t* payload, size_t len, uint32_t param3,
                    uint32_t param4, std::string* err) override {
    std::vector<std::vector<uint8_t>> reports;
    if (!build_reports(payload, len, report_id_, report_size_,
                       header_per_chunk_, param3, param4, &reports, err,
                       chunk_budget_, outer_framing(), ugreen_type_,
                       ugreen_flags_, params_every_report_)) {
      return false;
    }
    reports_sent_ += reports.size();
    ++messages_sent_;
    return true;
  }
  bool recv_message(std::string*, int, bool* timed_out,
                    std::string*) override {
    if (timed_out) *timed_out = true;
    return false;
  }
  std::string describe() const override { return "null-hid-report"; }
  uint8_t report_id() const override { return report_id_; }
  size_t report_size() const override { return report_size_; }
  size_t payload_budget() const override {
    return report_size_ > kOuterHeaderSize ? report_size_ - kOuterHeaderSize : 0;
  }
  size_t reports_sent() const override { return reports_sent_; }
  size_t messages_sent() const override { return messages_sent_; }

 private:
  uint8_t report_id_;
  size_t report_size_;
  bool header_per_chunk_ = false;
  bool open_ = false;
  size_t reports_sent_ = 0;
  size_t messages_sent_ = 0;
  size_t chunk_budget_ = 0;
};

}  // namespace

std::unique_ptr<ReportSink> make_hidraw_report_sink(const std::string& path,
                                                    uint8_t report_id,
                                                    size_t report_size,
                                                    bool header_per_chunk,
                                                    size_t chunk_budget) {
  return std::unique_ptr<ReportSink>(new HidrawReportSink(
      path, report_id, report_size, header_per_chunk, chunk_budget));
}

std::unique_ptr<ReportSink> make_recording_report_sink(const std::string& path,
                                                       uint8_t report_id,
                                                       size_t report_size,
                                                       bool header_per_chunk,
                                                       size_t chunk_budget) {
  return std::unique_ptr<ReportSink>(new RecordingReportSink(
      path, report_id, report_size, header_per_chunk, chunk_budget));
}

std::unique_ptr<ReportSink> make_null_report_sink(uint8_t report_id,
                                                  size_t report_size,
                                                  size_t chunk_budget) {
  return std::unique_ptr<ReportSink>(
      new NullReportSink(report_id, report_size, chunk_budget));
}

}  // namespace hid
}  // namespace usbdisplay
