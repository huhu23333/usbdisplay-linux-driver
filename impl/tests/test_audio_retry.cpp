// SPDX-License-Identifier: MIT
#include <string>
#include <vector>

#include "audio/audio_open_retry.h"
#include "test_harness.h"

int main() {
  using namespace usbdisplay::audio;
  {
    // 前两次失败，第三次成功，重试间隔为 200ms。
    OpenRetryPolicy policy;
    policy.max_attempts = 5;
    policy.interval_ms = 200;
    int calls = 0;
    std::vector<int> sleeps;
    OpenRetryResult result = open_with_retry(
        policy,
        [&](std::string* err) {
          ++calls;
          if (calls < 3) {
            if (err) *err = "声卡未就绪";
            return false;
          }
          return true;
        },
        [&](int interval_ms) { sleeps.push_back(interval_ms); });
    CHECK(result.ok);
    CHECK(result.attempts == 3);
    CHECK(calls == 3);
    CHECK(sleeps.size() == 2);
    CHECK(sleeps[0] == 200 && sleeps[1] == 200);
  }
  {
    // 达到最大次数仍失败时返回最后一次错误。
    OpenRetryPolicy policy;
    policy.max_attempts = 3;
    policy.interval_ms = 200;
    int calls = 0;
    std::vector<int> sleeps;
    OpenRetryResult result = open_with_retry(
        policy,
        [&](std::string* err) {
          ++calls;
          if (err) *err = "设备忙";
          return false;
        },
        [&](int interval_ms) { sleeps.push_back(interval_ms); });
    CHECK(!result.ok);
    CHECK(result.attempts == 3);
    CHECK(calls == 3);
    CHECK(sleeps.size() == 2);
    CHECK(result.error == "设备忙");
  }
  {
    // 首次成功不等待。
    OpenRetryPolicy policy;
    policy.max_attempts = 3;
    int sleeps = 0;
    OpenRetryResult result = open_with_retry(
        policy, [](std::string*) { return true; },
        [&](int) { ++sleeps; });
    CHECK(result.ok);
    CHECK(result.attempts == 1);
    CHECK(sleeps == 0);
  }
  return testharness::finish("test_audio_retry");
}
