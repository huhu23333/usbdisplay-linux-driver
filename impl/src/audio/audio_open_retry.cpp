// SPDX-License-Identifier: MIT
#include "audio/audio_open_retry.h"

namespace usbdisplay {
namespace audio {

OpenRetryResult open_with_retry(const OpenRetryPolicy& policy,
                                const OpenAttempt& attempt,
                                const RetrySleep& sleep_for) {
  OpenRetryResult result;
  if (!attempt) {
    result.error = "打开音频设备的回调为空";
    return result;
  }
  const int max_attempts = policy.max_attempts > 0 ? policy.max_attempts : 1;
  for (int attempt_index = 0; attempt_index < max_attempts; ++attempt_index) {
    result.attempts = attempt_index + 1;
    std::string attempt_error;
    if (attempt(&attempt_error)) {
      result.ok = true;
      result.error.clear();
      return result;
    }
    result.error = attempt_error;
    if (attempt_index + 1 < max_attempts && sleep_for) {
      sleep_for(policy.interval_ms);
    }
  }
  return result;
}

}  // namespace audio
}  // namespace usbdisplay
