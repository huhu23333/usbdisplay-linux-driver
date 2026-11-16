// SPDX-License-Identifier: MIT
#include "transport/retry.h"

namespace usbdisplay {
namespace transport {

ResetRetryResult run_with_auto_reset(const ResetRetryPolicy& policy,
                                     const ResetRetryCallbacks& callbacks) {
  ResetRetryResult result;
  if (!callbacks.attempt) {
    result.error = "no attempt callback";
    return result;
  }
  for (int index = 0;; ++index) {
    result.attempts = index + 1;
    std::string attempt_error;
    if (callbacks.attempt(&attempt_error)) {
      result.ok = true;
      result.error.clear();
      return result;
    }
    result.error = attempt_error;
    if (!policy.auto_reset) return result;
    if (index >= policy.max_reset_attempts) return result;
    if (callbacks.is_fatal && callbacks.is_fatal(attempt_error)) return result;
    if (!callbacks.reset) return result;
    const int reset_number = index + 1;
    if (callbacks.log) callbacks.log(reset_number, attempt_error);
    std::string reset_error;
    if (!callbacks.reset(&reset_error)) {
      if (!reset_error.empty()) result.error = reset_error;
      return result;
    }
    ++result.resets;
  }
}

}  // namespace transport
}  // namespace usbdisplay
