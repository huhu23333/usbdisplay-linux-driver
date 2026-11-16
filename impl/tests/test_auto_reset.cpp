// SPDX-License-Identifier: MIT
#include <string>

#include "test_harness.h"
#include "transport/retry.h"

int main() {
  using namespace usbdisplay::transport;
  {
    // Fail twice, then succeed: two resets, three attempts.
    int attempts = 0;
    int resets = 0;
    int logs = 0;
    std::string last_logged;
    ResetRetryPolicy policy;
    ResetRetryCallbacks callbacks;
    callbacks.attempt = [&](std::string* err) -> bool {
      ++attempts;
      if (attempts < 3) {
        *err = "no isAllowed (attempt " + std::to_string(attempts) + ")";
        return false;
      }
      return true;
    };
    callbacks.reset = [&](std::string*) -> bool {
      ++resets;
      return true;
    };
    callbacks.log = [&](int reset_attempt, const std::string& error) {
      ++logs;
      last_logged = std::to_string(reset_attempt) + ":" + error;
    };
    const ResetRetryResult result = run_with_auto_reset(policy, callbacks);
    CHECK(result.ok);
    CHECK(result.attempts == 3);
    CHECK(result.resets == 2);
    CHECK(attempts == 3 && resets == 2 && logs == 2);
    CHECK(last_logged.find("2:") == 0);
    CHECK(last_logged.find("isAllowed") != std::string::npos);
  }
  {
    // auto_reset disabled: single attempt, no reset.
    int attempts = 0;
    int resets = 0;
    ResetRetryPolicy policy;
    policy.auto_reset = false;
    ResetRetryCallbacks callbacks;
    callbacks.attempt = [&](std::string* err) -> bool {
      ++attempts;
      *err = "handshake failed";
      return false;
    };
    callbacks.reset = [&](std::string*) -> bool {
      ++resets;
      return true;
    };
    const ResetRetryResult result = run_with_auto_reset(policy, callbacks);
    CHECK(!result.ok);
    CHECK(result.attempts == 1 && result.resets == 0);
    CHECK(attempts == 1 && resets == 0);
    CHECK(result.error == "handshake failed");
  }
  {
    // Exhausted retries: initial attempt + max_reset_attempts resets.
    int attempts = 0;
    int resets = 0;
    ResetRetryPolicy policy;
    policy.max_reset_attempts = 2;
    ResetRetryCallbacks callbacks;
    callbacks.attempt = [&](std::string* err) -> bool {
      ++attempts;
      *err = "claim busy";
      return false;
    };
    callbacks.reset = [&](std::string*) -> bool {
      ++resets;
      return true;
    };
    const ResetRetryResult result = run_with_auto_reset(policy, callbacks);
    CHECK(!result.ok);
    CHECK(result.attempts == 3);
    CHECK(result.resets == 2);
    CHECK(attempts == 3 && resets == 2);
    CHECK(result.error == "claim busy");
  }
  {
    // Fatal error (e.g. permission denied) stops immediately without reset.
    int resets = 0;
    ResetRetryPolicy policy;
    ResetRetryCallbacks callbacks;
    callbacks.attempt = [](std::string* err) -> bool {
      *err = "permission denied opening USB device; install the udev rule";
      return false;
    };
    callbacks.reset = [&](std::string*) -> bool {
      ++resets;
      return true;
    };
    callbacks.is_fatal = [](const std::string& error) -> bool {
      return error.find("permission") != std::string::npos;
    };
    const ResetRetryResult result = run_with_auto_reset(policy, callbacks);
    CHECK(!result.ok);
    CHECK(result.attempts == 1);
    CHECK(resets == 0);
  }
  {
    // Reset itself fails: report the reset error, stop the loop.
    int attempts = 0;
    int resets = 0;
    ResetRetryPolicy policy;
    ResetRetryCallbacks callbacks;
    callbacks.attempt = [&](std::string* err) -> bool {
      ++attempts;
      *err = "first attempt failed";
      return false;
    };
    callbacks.reset = [&](std::string* err) -> bool {
      ++resets;
      *err = "device did not re-enumerate";
      return false;
    };
    const ResetRetryResult result = run_with_auto_reset(policy, callbacks);
    CHECK(!result.ok);
    CHECK(result.attempts == 1);
    CHECK(attempts == 1 && resets == 1);
    CHECK(result.error == "device did not re-enumerate");
  }
  {
    // Missing reset callback: no retry.
    ResetRetryPolicy policy;
    ResetRetryCallbacks callbacks;
    callbacks.attempt = [](std::string* err) -> bool {
      *err = "failed";
      return false;
    };
    const ResetRetryResult result = run_with_auto_reset(policy, callbacks);
    CHECK(!result.ok);
    CHECK(result.attempts == 1);
  }
  return testharness::finish("test_auto_reset");
}
