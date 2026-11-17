// SPDX-License-Identifier: MIT
#include "evdi/state.h"
#include "test_harness.h"

int main() {
  using namespace usbdisplay::evdi;
  UsbSessionStateMachine machine;
  CHECK(machine.state() == UsbSessionState::Idle);
  CHECK(!machine.should_attempt(0));

  machine.set_usb_present(true, 0);
  CHECK(machine.should_attempt(0));
  machine.on_start_success();
  CHECK(machine.state() == UsbSessionState::Streaming);
  CHECK(!machine.should_attempt(1000));

  machine.on_usb_removed();
  CHECK(machine.state() == UsbSessionState::Idle);
  CHECK(!machine.usb_present());

  machine.set_usb_present(true, 1000);
  CHECK(machine.should_attempt(1000));
  machine.on_start_failure(1000);
  CHECK(machine.state() == UsbSessionState::Backoff);
  CHECK(machine.failures() == 1);
  CHECK(machine.backoff_ms() == 2000);
  CHECK(!machine.should_attempt(2999));
  CHECK(machine.should_attempt(3000));

  machine.on_start_failure(3000);
  CHECK(machine.failures() == 2);
  CHECK(machine.backoff_ms() == 2500);
  CHECK(!machine.should_attempt(5499));
  CHECK(machine.should_attempt(5500));

  for (int i = 0; i < 10; ++i) machine.on_start_failure(10000);
  CHECK(machine.backoff_ms() == 5000);

  machine.on_start_success();
  CHECK(machine.state() == UsbSessionState::Streaming);
  CHECK(machine.failures() == 0);
  return testharness::finish("test_daemon_state");
}
