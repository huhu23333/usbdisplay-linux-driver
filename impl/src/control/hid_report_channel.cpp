// SPDX-License-Identifier: MIT
#include "control/hid_report_channel.h"

#include "common/log.h"

namespace usbdisplay {
namespace control {

HidReportControlChannel::HidReportControlChannel(
    std::unique_ptr<hid::ReportSink> sink, uint32_t param3, uint32_t param4)
    : sink_(std::move(sink)), param3_(param3), param4_(param4) {}
HidReportControlChannel::~HidReportControlChannel() = default;

bool HidReportControlChannel::open(std::string* err) {
  return sink_ != nullptr && sink_->open(err);
}
void HidReportControlChannel::close() {
  if (sink_) sink_->close();
}
bool HidReportControlChannel::connected() const {
  return sink_ != nullptr && sink_->connected();
}

bool HidReportControlChannel::send_json(const std::string& json,
                                        std::string* err) {
  if (!sink_) {
    if (err) *err = "hid control sink missing";
    return false;
  }
  log::debug("control tx: " + json.substr(0, 256));
  return sink_->send_message(reinterpret_cast<const uint8_t*>(json.data()),
                             json.size(), param3_, param4_, err);
}

bool HidReportControlChannel::recv_json(std::string* json, int timeout_ms,
                                        bool* timed_out, std::string* err) {
  if (!sink_) {
    if (err) *err = "hid control sink missing";
    return false;
  }
  std::string payload;
  if (!sink_->recv_message(&payload, timeout_ms, timed_out, err)) return false;
  *json = std::move(payload);
  log::debug("control rx: " + json->substr(0, 256));
  return true;
}

std::string HidReportControlChannel::describe() const {
  return "hid-report-control " + (sink_ ? sink_->describe() : std::string("?"));
}

}  // namespace control
}  // namespace usbdisplay
