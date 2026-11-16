// SPDX-License-Identifier: MIT
// Generic "attempt -> reset -> retry" state machine used for the USB session
// auto-reset logic. Kept independent of libusb so it can be unit tested.
#pragma once

#include <functional>
#include <string>

namespace usbdisplay {
namespace transport {

struct ResetRetryPolicy {
  bool auto_reset = true;
  // Number of reset+retry cycles after the initial attempt (verified flow uses
  // 3; total attempts = max_reset_attempts + 1).
  int max_reset_attempts = 3;
  // How long to wait for the device to re-enumerate after a reset.
  int reset_wait_ms = 5000;
};

struct ResetRetryCallbacks {
  // One open+handshake attempt. Returns false and sets the error on failure.
  std::function<bool(std::string*)> attempt;
  // Reset + re-discover step between attempts.
  std::function<bool(std::string*)> reset;
  // Called before each reset: (reset_attempt_number, last_attempt_error).
  std::function<void(int reset_attempt, const std::string& last_error)> log;
  // Optional: return true for errors where resetting cannot help (e.g.
  // permission denied), stopping the retry loop immediately.
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
