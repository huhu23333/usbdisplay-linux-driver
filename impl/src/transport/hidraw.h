// SPDX-License-Identifier: MIT
// /dev/hidraw access for the USB Display HID control channel (no hidapi).
#pragma once

#include <cstddef>
#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace usbdisplay {
namespace transport {

struct HidrawInfo {
  std::string path;             // /dev/hidrawN
  std::string sysfs;            // /sys/class/hidraw/hidrawN
  std::string name;             // HID_NAME
  std::string phys;             // HID_PHYS
  std::string uniq;             // HID_UNIQ
  uint16_t bustype = 0;
  uint16_t vid = 0;
  uint16_t pid = 0;
  int interface_number = -1;    // USB bInterfaceNumber, -1 if unknown
  bool readable = false;
  bool writable = false;
};

std::vector<HidrawInfo> enumerate_hidraw(std::string* err);
bool find_hidraw(uint16_t vid, uint16_t pid, HidrawInfo* out, std::string* err);
// Find the hidraw node whose USB interface number matches (bInterfaceNumber).
bool find_hidraw_by_interface(uint16_t vid, uint16_t pid, int interface_number,
                              HidrawInfo* out, std::string* err);

// Poll for a (re-)enumerated, read/write-accessible hidraw node for the given
// USB interface. Used after a device reset.
bool wait_for_hidraw_interface(uint16_t vid, uint16_t pid, int interface_number,
                               int timeout_ms, HidrawInfo* out,
                               std::string* err);

// Report sizes decoded from a HID report descriptor, per report id, in bytes
// of report data (excluding the report-id byte).
struct HidReportSizes {
  std::map<uint8_t, size_t> input;
  std::map<uint8_t, size_t> output;
  std::map<uint8_t, size_t> feature;
};
bool parse_hid_report_descriptor(const std::vector<uint8_t>& descriptor,
                                 HidReportSizes* out, std::string* err);

class HidrawDevice {
 public:
  HidrawDevice() = default;
  ~HidrawDevice();
  HidrawDevice(const HidrawDevice&) = delete;
  HidrawDevice& operator=(const HidrawDevice&) = delete;

  bool open(const std::string& path, std::string* err);
  void close();
  bool is_open() const { return fd_ >= 0; }
  int fd() const { return fd_; }
  const std::string& path() const { return path_; }

  bool get_info(HidrawInfo* out, std::string* err) const;
  std::vector<uint8_t> report_descriptor(std::string* err) const;

  // Writes an HID output report. `payload` excludes the report-id byte;
  // the buffer is padded to report_size bytes of payload.
  bool write_output_report(uint8_t report_id, const uint8_t* payload,
                           size_t len, size_t report_size, std::string* err);
  // Same but via HIDIOCSFEATURE (feature report).
  bool write_feature_report(uint8_t report_id, const uint8_t* payload,
                            size_t len, size_t report_size, std::string* err);
  // Reads one input report. Returns >0 = bytes, 0 = timeout, -1 = error.
  int read_report(uint8_t* buf, size_t len, int timeout_ms, std::string* err);

  // Writes exactly one HID report buffer verbatim (buffer[0] = report id,
  // length must equal the report size expected by the kernel).
  bool write_raw(const uint8_t* buffer, size_t len, std::string* err);

 private:
  int fd_ = -1;
  std::string path_;
};

}  // namespace transport
}  // namespace usbdisplay
