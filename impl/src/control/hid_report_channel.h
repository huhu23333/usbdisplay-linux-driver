// SPDX-License-Identifier: MIT
// Control channel over the IF0 HID report sink (JSON-RPC 2.0 commands).
#pragma once

#include <cstdint>
#include <memory>
#include <string>

#include "control/control_channel.h"
#include "hid/report_sink.h"

namespace usbdisplay {
namespace control {

class HidReportControlChannel : public ControlChannel {
 public:
  HidReportControlChannel(std::unique_ptr<hid::ReportSink> sink,
                          uint32_t param3 = 0, uint32_t param4 = 0);
  ~HidReportControlChannel() override;

  bool open(std::string* err) override;
  void close() override;
  bool connected() const override;
  bool send_json(const std::string& json, std::string* err) override;
  bool recv_json(std::string* json, int timeout_ms, bool* timed_out,
                 std::string* err) override;
  std::string describe() const override;

  hid::ReportSink* sink() { return sink_.get(); }

 private:
  std::unique_ptr<hid::ReportSink> sink_;
  uint32_t param3_;
  uint32_t param4_;
};

}  // namespace control
}  // namespace usbdisplay
