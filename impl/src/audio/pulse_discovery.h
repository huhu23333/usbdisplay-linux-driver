// SPDX-License-Identifier: MIT
// PulseAudio 输出设备发现、默认 sink 读取与恢复。
#pragma once

#include <memory>
#include <string>
#include <vector>

namespace usbdisplay {
namespace audio {

struct PulseSinkInfo {
  std::string name;
  std::string description;
};

std::string select_usb_sink(const std::vector<PulseSinkInfo>& sinks);

class PulseDiscovery {
 public:
  virtual ~PulseDiscovery() = default;
  virtual bool list_sinks(std::vector<PulseSinkInfo>* out, std::string* err) = 0;
  virtual bool get_default_sink(std::string* out, std::string* err) = 0;
  virtual bool set_default_sink(const std::string& name, std::string* err) = 0;
};

// 使用 libpulse 主循环同步查询与设置默认 sink。
std::unique_ptr<PulseDiscovery> make_libpulse_discovery();

}  // namespace audio
}  // namespace usbdisplay
