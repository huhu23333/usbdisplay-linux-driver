// SPDX-License-Identifier: MIT
#include "transport/libusb_probe.h"

#include <libusb-1.0/libusb.h>

#include <cstdio>
#include <cstring>

#include "common/log.h"

namespace usbdisplay {
namespace transport {
namespace {

std::string libusb_string(libusb_device_handle* handle, uint8_t index) {
  if (index == 0) return "";
  char buf[256];
  const int n = ::libusb_get_string_descriptor_ascii(
      handle, index, reinterpret_cast<unsigned char*>(buf), sizeof(buf));
  if (n <= 0) return "";
  return std::string(buf, static_cast<size_t>(n));
}

}  // namespace

const char* endpoint_type_name(uint8_t type) {
  switch (type) {
    case LIBUSB_TRANSFER_TYPE_CONTROL: return "control";
    case LIBUSB_TRANSFER_TYPE_ISOCHRONOUS: return "isochronous";
    case LIBUSB_TRANSFER_TYPE_BULK: return "bulk";
    case LIBUSB_TRANSFER_TYPE_INTERRUPT: return "interrupt";
  }
  return "?";
}

const char* endpoint_direction(uint8_t address) {
  return (address & 0x80) ? "IN" : "OUT";
}

std::vector<UsbDeviceInfo> enumerate_usb(std::string* err) {
  std::vector<UsbDeviceInfo> out;
  libusb_context* ctx = nullptr;
  int rc = ::libusb_init(&ctx);
  if (rc != 0) {
    if (err) *err = std::string("libusb_init: ") + ::libusb_strerror(rc);
    return out;
  }
  libusb_device** list = nullptr;
  const ssize_t count = ::libusb_get_device_list(ctx, &list);
  if (count < 0) {
    if (err) *err = "libusb_get_device_list failed";
    ::libusb_exit(ctx);
    return out;
  }
  for (ssize_t i = 0; i < count; ++i) {
    libusb_device* dev = list[i];
    struct libusb_device_descriptor desc{};
    if (::libusb_get_device_descriptor(dev, &desc) != 0) continue;
    UsbDeviceInfo info;
    info.vid = desc.idVendor;
    info.pid = desc.idProduct;
    info.bcd_device = desc.bcdDevice;
    info.bus = ::libusb_get_bus_number(dev);
    info.address = ::libusb_get_device_address(dev);
    info.device_class = desc.bDeviceClass;
    libusb_device_handle* handle = nullptr;
    if (::libusb_open(dev, &handle) == 0 && handle != nullptr) {
      info.manufacturer = libusb_string(handle, desc.iManufacturer);
      info.product = libusb_string(handle, desc.iProduct);
      info.serial = libusb_string(handle, desc.iSerialNumber);
    } else {
      log::debug("usb: cannot open device for strings (permissions?)");
    }
    for (uint8_t ci = 0; ci < desc.bNumConfigurations; ++ci) {
      struct libusb_config_descriptor* config = nullptr;
      if (::libusb_get_config_descriptor(dev, ci, &config) != 0) continue;
      for (int ii = 0; ii < config->bNumInterfaces; ++ii) {
        const struct libusb_interface& iface = config->interface[ii];
        for (int ai = 0; ai < iface.num_altsetting; ++ai) {
          const struct libusb_interface_descriptor& idesc =
              iface.altsetting[ai];
          UsbInterfaceInfo ifinfo;
          ifinfo.number = idesc.bInterfaceNumber;
          ifinfo.alternate = idesc.bAlternateSetting;
          ifinfo.klass = idesc.bInterfaceClass;
          ifinfo.subclass = idesc.bInterfaceSubClass;
          ifinfo.protocol = idesc.bInterfaceProtocol;
          if (ifinfo.klass == LIBUSB_CLASS_HID) info.has_hid_interface = true;
          for (int ei = 0; ei < idesc.bNumEndpoints; ++ei) {
            const struct libusb_endpoint_descriptor& edesc =
                idesc.endpoint[ei];
            UsbEndpointInfo ep;
            ep.address = edesc.bEndpointAddress;
            ep.transfer_type =
                static_cast<uint8_t>(edesc.bmAttributes & 0x03);
            ep.max_packet_size = edesc.wMaxPacketSize;
            ep.interval = edesc.bInterval;
            ifinfo.endpoints.push_back(ep);
          }
          if (handle != nullptr) {
            ifinfo.claimed_by_kernel_driver =
                ::libusb_kernel_driver_active(handle, idesc.bInterfaceNumber) == 1;
          }
          info.interfaces.push_back(ifinfo);
        }
      }
      ::libusb_free_config_descriptor(config);
    }
    if (handle != nullptr) ::libusb_close(handle);
    out.push_back(std::move(info));
  }
  ::libusb_free_device_list(list, 1);
  ::libusb_exit(ctx);
  return out;
}

bool usb_device_accessible(uint16_t vid, uint16_t pid, std::string* err) {
  libusb_context* ctx = nullptr;
  if (::libusb_init(&ctx) != 0) {
    if (err) *err = "libusb_init failed";
    return false;
  }
  libusb_device** list = nullptr;
  const ssize_t count = ::libusb_get_device_list(ctx, &list);
  bool accessible = false;
  for (ssize_t i = 0; i < count; ++i) {
    struct libusb_device_descriptor desc{};
    if (::libusb_get_device_descriptor(list[i], &desc) != 0) continue;
    if (desc.idVendor != vid || desc.idProduct != pid) continue;
    libusb_device_handle* handle = nullptr;
    const int rc = ::libusb_open(list[i], &handle);
    if (rc == 0 && handle != nullptr) {
      accessible = true;
      ::libusb_close(handle);
    } else if (err) {
      char buf[384];
      std::snprintf(
          buf, sizeof(buf),
          "cannot open %04x:%04x (bus %u addr %u): %s; install the one-time "
          "udev rule: sudo cp tools/99-usbdisplay.rules /etc/udev/rules.d/ && "
          "sudo udevadm control --reload && sudo udevadm trigger (fallback "
          "for this session: sudo chmod 666 /dev/bus/usb/%03u/%03u)",
          vid, pid, ::libusb_get_bus_number(list[i]),
          ::libusb_get_device_address(list[i]), ::libusb_strerror(rc),
          ::libusb_get_bus_number(list[i]),
          ::libusb_get_device_address(list[i]));
      *err = buf;
    }
    break;
  }
  if (count > 0) ::libusb_free_device_list(list, 1);
  ::libusb_exit(ctx);
  if (!accessible && err && err->empty()) {
    *err = "USB device not found or not accessible";
  }
  return accessible;
}

bool find_usb_device(uint16_t vid, uint16_t pid, UsbDeviceInfo* out,
                     std::string* err) {
  const auto all = enumerate_usb(err);
  for (const auto& info : all) {
    if (info.vid == vid && info.pid == pid) {
      *out = info;
      return true;
    }
  }
  if (err) {
    char buf[64];
    std::snprintf(buf, sizeof(buf), "VID_%04X&PID_%04X", vid, pid);
    *err = std::string("USB device ") + buf + " not found";
  }
  return false;
}

}  // namespace transport
}  // namespace usbdisplay
