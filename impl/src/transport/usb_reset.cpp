// SPDX-License-Identifier: MIT
#include "transport/usb_reset.h"

#include <libusb-1.0/libusb.h>
#include <unistd.h>

#include <chrono>
#include <cstdio>
#include <thread>

#include "common/log.h"
#include "common/util.h"

namespace usbdisplay {
namespace transport {
namespace {

libusb_device* find_device(libusb_context* context, libusb_device*** list,
                           ssize_t* count, uint16_t vid, uint16_t pid) {
  *count = ::libusb_get_device_list(context, list);
  if (*count < 0) return nullptr;
  libusb_device** devices = *list;
  for (ssize_t i = 0; i < *count; ++i) {
    struct libusb_device_descriptor desc{};
    if (::libusb_get_device_descriptor(devices[i], &desc) != 0) continue;
    if (desc.idVendor == vid && desc.idProduct == pid) return devices[i];
  }
  return nullptr;
}

}  // namespace

bool usb_reset_device(uint16_t vid, uint16_t pid, std::string* err) {
  libusb_context* context = nullptr;
  if (::libusb_init(&context) != 0) {
    if (err) *err = "libusb_init failed";
    return false;
  }
  libusb_device** list = nullptr;
  ssize_t count = 0;
  libusb_device* device = find_device(context, &list, &count, vid, pid);
  if (device == nullptr) {
    if (err) {
      char buf[96];
      std::snprintf(buf, sizeof(buf),
                    "USB device %04x:%04x not found for reset", vid, pid);
      *err = buf;
    }
    if (count > 0) ::libusb_free_device_list(list, 1);
    ::libusb_exit(context);
    return false;
  }
  libusb_device_handle* handle = nullptr;
  int rc = ::libusb_open(device, &handle);
  if (rc != 0 || handle == nullptr) {
    if (err) {
      char buf[320];
      std::snprintf(buf, sizeof(buf),
                    "cannot open %04x:%04x for reset: %s; install the udev "
                    "rule: sudo tools/install-video-rules.sh",
                    vid, pid, ::libusb_strerror(rc));
      *err = buf;
    }
    if (count > 0) ::libusb_free_device_list(list, 1);
    ::libusb_exit(context);
    return false;
  }
  const int address = ::libusb_get_device_address(device);
  log::info("usb-reset: resetting " + std::to_string(vid) + ":" +
            std::to_string(pid) + " (addr " + std::to_string(address) + ")");
  rc = ::libusb_reset_device(handle);
  ::libusb_close(handle);
  if (count > 0) ::libusb_free_device_list(list, 1);
  ::libusb_exit(context);
  if (rc != 0) {
    if (err) {
      *err = std::string("libusb_reset_device: ") + ::libusb_strerror(rc);
    }
    return false;
  }
  // 等待内核移除旧设备节点后开始轮询重新枚举。
  std::this_thread::sleep_for(std::chrono::milliseconds(300));
  return true;
}

bool wait_for_usb_device(uint16_t vid, uint16_t pid, int timeout_ms,
                         std::string* err) {
  libusb_context* context = nullptr;
  if (::libusb_init(&context) != 0) {
    if (err) *err = "libusb_init failed";
    return false;
  }
  const uint64_t start = mono_ms();
  bool disappeared = false;
  int first_address = -1;
  for (;;) {
    libusb_device** list = nullptr;
    ssize_t count = 0;
    libusb_device* device = find_device(context, &list, &count, vid, pid);
    int address = -1;
    if (device != nullptr) address = ::libusb_get_device_address(device);
    if (count > 0) ::libusb_free_device_list(list, 1);
    if (address < 0) {
      disappeared = true;
    } else {
      if (first_address < 0) first_address = address;
      if (disappeared || address != first_address ||
          mono_ms() - start > 500) {
        log::info("usb-reset: device re-enumerated (addr " +
                  std::to_string(address) + ")");
        ::libusb_exit(context);
        return true;
      }
    }
    if (mono_ms() - start >= static_cast<uint64_t>(timeout_ms)) break;
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
  }
  ::libusb_exit(context);
  if (err) {
    *err = "device did not re-enumerate within " + std::to_string(timeout_ms) +
           " ms";
  }
  return false;
}

}  // namespace transport
}  // namespace usbdisplay
