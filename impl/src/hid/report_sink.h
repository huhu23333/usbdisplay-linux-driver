// SPDX-License-Identifier: MIT
// HID report sinks: real /dev/hidraw, recording (offline dump) and null.
// A sink sends one logical message as N fixed-size HID reports; payloads are
// framed per hid_profile.h.
#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace usbdisplay {
namespace hid {

// Outer report framing. App16 is verified on hardware; Ugreen14 is the
// alternative "mode B" framing being trialled (14-byte little-endian header).
enum class OuterFraming : uint8_t { App16 = 0, Ugreen14 = 1 };

class ReportSink {
 public:
  virtual ~ReportSink() = default;
  virtual bool open(std::string* err) = 0;
  virtual void close() = 0;
  virtual bool connected() const = 0;
  // payload is the logical message (e.g. JSON text or a video inner message).
  virtual bool send_message(const uint8_t* payload, size_t len,
                            uint32_t param3, uint32_t param4,
                            std::string* err) = 0;
  // Receives one logical message payload. timed_out=true when no data arrived.
  virtual bool recv_message(std::string* payload, int timeout_ms,
                            bool* timed_out, std::string* err) = 0;
  virtual std::string describe() const = 0;
  virtual uint8_t report_id() const = 0;
  virtual size_t report_size() const = 0;
  virtual size_t payload_budget() const = 0;
  virtual size_t reports_sent() const = 0;
  virtual size_t messages_sent() const = 0;

  // Select the alternative outer framing for subsequent messages.
  void set_ugreen_outer(uint8_t type, uint16_t flags) {
    ugreen_outer_ = true;
    ugreen_type_ = type;
    ugreen_flags_ = flags;
  }
  // App16: write param3/param4 in every report (default: first report only,
  // matching the app's SendHIDVideo).
  void set_params_every_report(bool every) { params_every_report_ = every; }

 protected:
  OuterFraming outer_framing() const {
    return ugreen_outer_ ? OuterFraming::Ugreen14 : OuterFraming::App16;
  }
  bool ugreen_outer_ = false;
  uint8_t ugreen_type_ = 0;
  uint16_t ugreen_flags_ = 0;
  bool params_every_report_ = false;
};

std::unique_ptr<ReportSink> make_hidraw_report_sink(const std::string& path,
                                                    uint8_t report_id,
                                                    size_t report_size,
                                                    bool header_per_chunk,
                                                    size_t chunk_budget = 0);
std::unique_ptr<ReportSink> make_recording_report_sink(const std::string& path,
                                                       uint8_t report_id,
                                                       size_t report_size,
                                                       bool header_per_chunk,
                                                       size_t chunk_budget = 0);
std::unique_ptr<ReportSink> make_null_report_sink(uint8_t report_id,
                                                  size_t report_size,
                                                  size_t chunk_budget = 0);

// Exposed for tests and tools: split a message into exact HID report buffers.
// App16 layout (verified): every report has [rid][BE32 total payload len]
// [BE32 param3][BE32 param4][3x00] + payload chunk at offset 16; param3/param4
// are written in the first report only unless params_every_report is set.
// For Ugreen14, param3/param4 carry the u32 seq and u32 fraginfo.
bool build_reports(const uint8_t* payload, size_t len, uint8_t report_id,
                   size_t report_size, bool header_per_chunk,
                   uint32_t param3, uint32_t param4,
                   std::vector<std::vector<uint8_t>>* out, std::string* err,
                   size_t chunk_budget = 0,
                   OuterFraming framing = OuterFraming::App16,
                   uint8_t ugreen_type = 0, uint16_t ugreen_flags = 0,
                   bool params_every_report = false);

}  // namespace hid
}  // namespace usbdisplay
