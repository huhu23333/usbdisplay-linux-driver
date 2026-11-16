// SPDX-License-Identifier: MIT
// 通用的尝试、复位、重试状态机，供 USB 会话自动复位使用。
// 该模块不依赖 libusb，便于进行单元测试。
#pragma once

#include <functional>
#include <string>

namespace usbdisplay {
namespace transport {

struct ResetRetryPolicy {
  bool auto_reset = true;
  // 首次尝试之后允许的复位重试次数，总尝试次数为该值加一。
  int max_reset_attempts = 3;
  // 复位后等待设备重新枚举的时长。
  int reset_wait_ms = 5000;
};

struct ResetRetryCallbacks {
  // 执行一次打开与握手尝试，失败时返回假并写入错误信息。
  std::function<bool(std::string*)> attempt;
  // 两次尝试之间的复位与重新发现步骤。
  std::function<bool(std::string*)> reset;
  // 每次复位前调用，参数为复位序号与上一次失败原因。
  std::function<void(int reset_attempt, const std::string& last_error)> log;
  // 可选回调，对无法通过复位解决的错误返回真以立即停止重试。
  std::function<bool(const std::string& error)> is_fatal;
};

struct ResetRetryResult {
  bool ok = false;
  int attempts = 0;  // total open+handshake attempts (>=1)
  int resets = 0;    // reset operations performed
  std::string error;
};

ResetRetryResult run_with_auto_reset(const ResetRetryPolicy& policy,
                                     const ResetRetryCallbacks& callbacks);

}  // namespace transport
}  // namespace usbdisplay
