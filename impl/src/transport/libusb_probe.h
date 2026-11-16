// SPDX-License-Identifier: MIT
// Device discovery / descriptor dump via libusb. Read-only; the actual control
// channel uses /dev/hidraw because the kernel HID driver normally owns the
// interface (detaching it would need root on most systems).
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace usbdisplay {
namespace transport {

struct UsbEndpointInfo {
  uint8_t address = 0;
  uint8_t transfer_type = 0;  // 0 control 1 iso 2 bulk 3 interrupt
  uint16_t max_packet_size = 0;
  uint8_t interval = 0;
};

struct UsbInterfaceInfo {
  int number = -1;
  int alternate = 0;
  uint8_t klass = 0;
  uint8_t subclass = 0;
  uint8_t protocol = 0;
  bool claimed_by_kernel_driver = false;
  std::vector<UsbEndpointInfo> endpoints;
};

struct UsbDeviceInfo {
  uint16_t vid = 0;
  uint16_t pid = 0;
  uint16_t bcd_device = 0;
  uint8_t bus = 0;
  uint8_t address = 0;
  uint8_t device_class = 0;
  std::string manufacturer;
  std::string product;
  std::string serial;
  bool has_hid_interface = false;
  std::vector<UsbInterfaceInfo> interfaces;
};

std::vector<UsbDeviceInfo> enumerate_usb(std::string* err);
bool find_usb_device(uint16_t vid, uint16_t pid, UsbDeviceInfo* out,
                     std::string* err);

// Check whether this process can open the device (needed for the libusb EP2
// video path). err carries a chmod/udev hint on permission failure.
bool usb_device_accessible(uint16_t vid, uint16_t pid, std::string* err);

const char* endpoint_type_name(uint8_t type);
const char* endpoint_direction(uint8_t address);

}  // namespace transport
}  // namespace usbdisplay
