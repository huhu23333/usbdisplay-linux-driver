// SPDX-License-Identifier: MIT
// USB device reset / re-enumeration helpers for session restart.
//
// The receiver firmware only accepts a session after a fresh enumeration, so a
// libusb_reset_device() followed by re-discovery is used instead of asking the
// operator to unplug/replug (verified in work/hw/reset_test.py: after reset the
// device re-enumerates with a new address and heartbeats/app-launched get ACKed
// again).
#pragma once

#include <cstdint>
#include <string>

namespace usbdisplay {
namespace transport {

// Opens the device (no interfaces claimed), calls libusb_reset_device and
// closes it. Any hidraw handles / claimed interfaces in this process must be
// closed first.
bool usb_reset_device(uint16_t vid, uint16_t pid, std::string* err);

// Waits until the device shows up again after a reset (it may briefly
// disappear and re-enumerate with a different USB address).
bool wait_for_usb_device(uint16_t vid, uint16_t pid, int timeout_ms,
                         std::string* err);

}  // namespace transport
}  // namespace usbdisplay
