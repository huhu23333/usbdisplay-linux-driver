// SPDX-License-Identifier: MIT
// 音频设备打开重试状态机，用于等待设备重新枚举后声卡就绪。
#pragma once

#include <functional>
#include <string>

namespace usbdisplay {
namespace audio {

struct OpenRetryPolicy {
  int max_attempts = 26;
  int interval_ms = 200;
};

using OpenAttempt = std::function<bool(std::string*)>;
using RetrySleep = std::function<void(int interval_ms)>;

struct OpenRetryResult {
  bool ok = false;
  int attempts = 0;
  std::string error;
};

OpenRetryResult open_with_retry(const OpenRetryPolicy& policy,
                                const OpenAttempt& attempt,
                                const RetrySleep& sleep_for);

}  // namespace audio
}  // namespace usbdisplay
