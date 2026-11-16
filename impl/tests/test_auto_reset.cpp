// SPDX-License-Identifier: MIT
#include <string>

#include "test_harness.h"
#include "transport/retry.h"

int main() {
  using namespace usbdisplay::transport;
  {
    // 前两次失败后成功，应执行两次复位、三次尝试。
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
    // 关闭自动复位时只尝试一次，不执行复位。
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
    // 重试次数耗尽时，总尝试次数为首次加上最大复位次数。
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
    // 权限错误等致命失败立即停止，不执行复位。
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
    // 复位本身失败时报告复位错误并停止循环。
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
    // 未提供复位回调时不重试。
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
