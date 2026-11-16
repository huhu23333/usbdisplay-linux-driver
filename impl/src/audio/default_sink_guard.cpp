// SPDX-License-Identifier: MIT
#include "audio/default_sink_guard.h"

#include <utility>

namespace usbdisplay {
namespace audio {

DefaultSinkGuard::DefaultSinkGuard(PulseDiscovery* control,
                                   std::string usb_sink,
                                   bool allow_restore)
    : control_(control),
      usb_sink_(std::move(usb_sink)),
      allow_restore_(allow_restore) {}

bool DefaultSinkGuard::capture(std::string* err) {
  captured_ = false;
  restored_ = false;
  failed_ = false;
  switch_observed_ = false;
  error_.clear();
  startup_default_.clear();
  if (control_ == nullptr || usb_sink_.empty()) {
    if (err) *err = "默认输出保护参数无效";
    return false;
  }
  std::string current;
  std::string read_err;
  if (!control_->get_default_sink(&current, &read_err) || current.empty()) {
    error_ = read_err.empty() ? "读取默认 sink 失败" : read_err;
    if (err) *err = error_;
    return false;
  }
  startup_default_ = current;
  captured_ = true;
  return true;
}

bool DefaultSinkGuard::capture_with_default(const std::string& startup_default,
                                            std::string* err) {
  captured_ = false;
  restored_ = false;
  failed_ = false;
  switch_observed_ = false;
  error_.clear();
  startup_default_.clear();
  if (control_ == nullptr || usb_sink_.empty() || startup_default.empty()) {
    if (err) *err = "默认输出保护参数无效";
    return false;
  }
  startup_default_ = startup_default;
  captured_ = true;
  return true;
}

bool DefaultSinkGuard::check_and_restore(std::string* err) {
  if (!captured_) {
    if (err) *err = error_.empty() ? "未记录启动默认 sink" : error_;
    return false;
  }
  if (startup_default_ == usb_sink_) {
    return true;
  }
  std::string current;
  std::string read_err;
  if (!control_->get_default_sink(&current, &read_err)) {
    failed_ = true;
    error_ = "读取当前默认 sink 失败：" + read_err;
    if (err) *err = error_;
    return false;
  }
  if (current != usb_sink_) {
    return true;
  }
  switch_observed_ = true;
  if (!allow_restore_) {
    return true;
  }
  std::string set_err;
  if (!control_->set_default_sink(startup_default_, &set_err)) {
    failed_ = true;
    error_ = "恢复默认 sink 失败：" + set_err;
    if (err) *err = error_;
    return false;
  }
  restored_ = true;
  return true;
}

bool DefaultSinkGuard::active() const {
  return captured_ && startup_default_ != usb_sink_;
}
bool DefaultSinkGuard::allow_restore() const { return allow_restore_; }
bool DefaultSinkGuard::restored() const { return restored_; }
bool DefaultSinkGuard::failed() const { return failed_; }
const std::string& DefaultSinkGuard::startup_default() const {
  return startup_default_;
}

std::string DefaultSinkGuard::exit_note() const {
  if (failed_) {
    return (allow_restore_ ? "默认输出恢复失败：" : "默认输出查询失败：") + error_;
  }
  if (!captured_) return "默认输出未修改（默认 sink 未知）";
  if (allow_restore_) {
    if (restored_) return "默认输出已恢复为 " + startup_default_;
    if (startup_default_ == usb_sink_) {
      return "默认输出未修改（开流前已是 USB 输出）";
    }
    return "默认输出未修改";
  }
  std::string note = "默认输出已记录：" + startup_default_ + "，未修改";
  if (startup_default_ == usb_sink_) {
    return "默认输出已记录：" + startup_default_ + "（开流前已是 USB 输出）";
  }
  if (switch_observed_) {
    note += "（当前已切换到 USB sink）";
  }
  return note;
}

}  // namespace audio
}  // namespace usbdisplay
