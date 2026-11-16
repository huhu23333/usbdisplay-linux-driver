// SPDX-License-Identifier: MIT
// Control channel over /dev/hidraw (HID output/feature reports).
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "control/control_channel.h"
#include "transport/hidraw.h"

namespace usbdisplay {
namespace control {

struct HidChannelConfig {
  std::string path;
  uint16_t vid = 0x1DE1;
  uint16_t pid = 0xF201;
  uint8_t report_id = 0;
  size_t report_size = 64;
  bool use_feature_report = false;
  // Provisional chunked framing for messages larger than one report:
  // [u16le total][u16le offset][chunk...]; see README.
  bool chunked = false;
  int timeout_ms = 1000;
};

class HidControlChannel : public ControlChannel {
 public:
  explicit HidControlChannel(HidChannelConfig config);
  ~HidControlChannel() override;

  bool open(std::string* err) override;
  void close() override;
  bool connected() const override;
  bool send_json(const std::string& json, std::string* err) override;
  bool recv_json(std::string* json, int timeout_ms, bool* timed_out,
                 std::string* err) override;
  std::string describe() const override;

  const transport::HidrawInfo& info() const { return info_; }

 private:
  HidChannelConfig config_;
  transport::HidrawDevice device_;
  transport::HidrawInfo info_;
  std::vector<uint8_t> rx_buffer_;
};

}  // namespace control
}  // namespace usbdisplay
