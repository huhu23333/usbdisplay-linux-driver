// SPDX-License-Identifier: MIT
// 默认输出保护：记录启动时的默认 sink，播放期间若被切到 USB sink 则恢复。
#pragma once

#include <string>

#include "audio/pulse_discovery.h"

namespace usbdisplay {
namespace audio {

class DefaultSinkGuard {
 public:
  // allow_restore 为假时只读取并记录默认 sink，不修改系统设置。
  DefaultSinkGuard(PulseDiscovery* control, std::string usb_sink,
                   bool allow_restore = false);

  // 读取并保存启动时的默认 sink。启动默认即为 USB sink 时不启用保护。
  bool capture(std::string* err);
  // 使用开流之前已经读取的默认 sink 值，避免 PulseAudio 自动切换后保存错误值。
  bool capture_with_default(const std::string& startup_default,
                            std::string* err);

  // 检查当前默认 sink，被切到 USB sink 时恢复为启动时的值。
  bool check_and_restore(std::string* err);

  bool active() const;
  bool allow_restore() const;
  bool restored() const;
  bool failed() const;
  const std::string& startup_default() const;

  // 退出时打印的一行说明。
  std::string exit_note() const;

 private:
  PulseDiscovery* control_;
  std::string usb_sink_;
  std::string startup_default_;
  bool allow_restore_ = false;
  bool captured_ = false;
  bool restored_ = false;
  bool failed_ = false;
  bool switch_observed_ = false;
  std::string error_;
};

}  // namespace audio
}  // namespace usbdisplay
