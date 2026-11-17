// SPDX-License-Identifier: MIT
#include "evdi/send_cadence.h"
#include "test_harness.h"
int main() {
  using usbdisplay::evdi::SendCadence;
  SendCadence cadence(60);
  CHECK(cadence.fps() == 60);
  CHECK(cadence.due(0));
  cadence.advance(0);
  CHECK(!cadence.due(16));
  CHECK(cadence.due(17));
  cadence.advance(17);
  CHECK(!cadence.due(33));
  CHECK(cadence.due(34));
  CHECK(cadence.pts_us(0) == 0);
  CHECK(cadence.pts_us(1) == 16666);
  CHECK(cadence.pts_us(60) == 1000000);
  int ticks = 0;
  for (uint64_t now = 0; now < 1000; ++now) {
    if (cadence.due(now)) { ++ticks; cadence.advance(now); }
  }
  CHECK(ticks <= 61);  // 1 秒内最多约 60 次发送
  return testharness::finish("test_send_cadence");
}
