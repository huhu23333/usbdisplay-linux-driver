// SPDX-License-Identifier: MIT
// 基于 hidraw 的控制通道，使用 HID 输出报告与特性报告。
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
  // 大于单条报告的消息使用分片格式：小端序总长度、小端序偏移、分片内容。
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
