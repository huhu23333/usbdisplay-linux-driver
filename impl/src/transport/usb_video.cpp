// SPDX-License-Identifier: MIT
#include "transport/usb_video.h"

#include <libusb-1.0/libusb.h>

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstring>

#include "common/log.h"
#include "common/util.h"
#include "hid/report_dump.h"

namespace usbdisplay {
namespace transport {
namespace {

constexpr size_t kUrbHeaderSize = 16;

std::string access_hint(libusb_device* device) {
  char buf[384];
  std::snprintf(
      buf, sizeof(buf),
      "permission denied opening USB device (bus %u addr %u); install the "
      "one-time udev rule: sudo tools/install-video-rules.sh "
      "(fallback for this session only: sudo chmod 666 "
      "/dev/bus/usb/%03u/%03u)",
      libusb_get_bus_number(device), libusb_get_device_address(device),
      libusb_get_bus_number(device), libusb_get_device_address(device));
  return buf;
}

class LibusbVideoSink : public UsbVideoSink {
 public:
  explicit LibusbVideoSink(const VideoUrbOptions& options) : options_(options) {}
  ~LibusbVideoSink() override { close(); }

  bool open(std::string* err) override {
    close();
    if (options_.urb_size == 0) {
      if (err) *err = "invalid URB size";
      return false;
    }
    int rc = libusb_init(&context_);
    if (rc != 0) {
      if (err) *err = std::string("libusb_init: ") + libusb_strerror(rc);
      return false;
    }
    libusb_device** list = nullptr;
    const ssize_t count = libusb_get_device_list(context_, &list);
    if (count < 0) {
      if (err) *err = "libusb_get_device_list failed";
      close();
      return false;
    }
    libusb_device* target = nullptr;
    for (ssize_t i = 0; i < count; ++i) {
      struct libusb_device_descriptor desc{};
      if (libusb_get_device_descriptor(list[i], &desc) != 0) continue;
      if (desc.idVendor == options_.vid && desc.idProduct == options_.pid) {
        target = list[i];
        break;
      }
    }
    if (target == nullptr) {
      if (err) {
        char buf[96];
        std::snprintf(buf, sizeof(buf),
                      "USB device %04x:%04x not found on the bus",
                      options_.vid, options_.pid);
        *err = buf;
      }
      libusb_free_device_list(list, 1);
      close();
      return false;
    }
    rc = libusb_open(target, &handle_);
    if (rc != 0) {
      if (err) {
        *err = rc == LIBUSB_ERROR_ACCESS
                   ? access_hint(target)
                   : std::string("libusb_open: ") + libusb_strerror(rc);
      }
      libusb_free_device_list(list, 1);
      close();
      return false;
    }
    const int iface = options_.interface_number;
    if (libusb_kernel_driver_active(handle_, iface) == 1) {
      rc = libusb_detach_kernel_driver(handle_, iface);
      if (rc != 0) {
        if (err) {
          *err = std::string("libusb_detach_kernel_driver(") +
                 std::to_string(iface) + "): " + libusb_strerror(rc) +
                 " (close any program using the video hidraw node)";
        }
        libusb_free_device_list(list, 1);
        close();
        return false;
      }
      detached_kernel_driver_ = true;
    }
    rc = libusb_claim_interface(handle_, iface);
    if (rc != 0) {
      if (err) {
        *err = std::string("libusb_claim_interface(") +
               std::to_string(iface) + "): " + libusb_strerror(rc);
      }
      libusb_free_device_list(list, 1);
      close();
      return false;
    }
    claimed_interface_ = true;

    // 查找视频接口的输出端点。
    struct libusb_config_descriptor* config = nullptr;
    endpoint_address_ = options_.endpoint_address;
    if (libusb_get_active_config_descriptor(target, &config) == 0 &&
        config != nullptr) {
      for (int i = 0; i < config->bNumInterfaces; ++i) {
        const struct libusb_interface& interface = config->interface[i];
        if (interface.num_altsetting == 0) continue;
        const struct libusb_interface_descriptor& desc =
            interface.altsetting[0];
        if (desc.bInterfaceNumber != iface) continue;
        for (int e = 0; e < desc.bNumEndpoints; ++e) {
          const struct libusb_endpoint_descriptor& ep = desc.endpoint[e];
          if ((ep.bEndpointAddress & 0x80) != 0) continue;  // OUT only
          const uint8_t type =
              static_cast<uint8_t>(ep.bmAttributes & 0x03);
          if (type != LIBUSB_TRANSFER_TYPE_INTERRUPT &&
              type != LIBUSB_TRANSFER_TYPE_BULK) {
            continue;
          }
          if (endpoint_address_ == 0 ||
              endpoint_address_ == ep.bEndpointAddress) {
            endpoint_address_ = ep.bEndpointAddress;
            if (type == LIBUSB_TRANSFER_TYPE_INTERRUPT) {
              mode_ = TransferMode::Interrupt;
              break;
            }
            if (mode_ == TransferMode::Auto) mode_ = TransferMode::Bulk;
          }
        }
      }
      libusb_free_config_descriptor(config);
    }
    libusb_free_device_list(list, 1);
    if (endpoint_address_ == 0) {
      if (err) *err = "no OUT endpoint found on USB interface 1";
      close();
      return false;
    }
    if (options_.transfer_mode == "interrupt") {
      mode_ = TransferMode::Interrupt;
      auto_mode_ = false;
    } else if (options_.transfer_mode == "bulk") {
      mode_ = TransferMode::Bulk;
      auto_mode_ = false;
    } else {
      auto_mode_ = true;
    }
    log::info("usb-video: opened " + describe() +
              " transfer=" + transfer_name());
    return true;
  }

  void close() override {
    if (handle_ != nullptr) {
      if (claimed_interface_) {
        libusb_release_interface(handle_, options_.interface_number);
        claimed_interface_ = false;
      }
      if (detached_kernel_driver_) {
        libusb_attach_kernel_driver(handle_, options_.interface_number);
        detached_kernel_driver_ = false;
      }
      libusb_close(handle_);
      handle_ = nullptr;
    }
    if (context_ != nullptr) {
      libusb_exit(context_);
      context_ = nullptr;
    }
  }

  bool connected() const override { return handle_ != nullptr; }

  bool send_frame(const uint8_t* annexb, size_t len, uint32_t width,
                  uint32_t height, std::string* err) override {
    if (handle_ == nullptr) {
      if (err) *err = "usb-video: not open";
      return false;
    }
    std::vector<std::vector<uint8_t>> urbs;
    if (!build_video_urbs(annexb, len, width, height, options_, &urbs, err)) {
      return false;
    }
    for (const auto& urb : urbs) {
      int transferred = 0;
      int rc = -1;
      for (int attempt = 0; attempt < 2; ++attempt) {
        const bool bulk = (mode_ == TransferMode::Bulk);
        if (bulk) {
          rc = libusb_bulk_transfer(handle_, endpoint_address_,
                                    const_cast<uint8_t*>(urb.data()),
                                    static_cast<int>(urb.size()), &transferred,
                                    options_.timeout_ms);
        } else {
          rc = libusb_interrupt_transfer(
              handle_, endpoint_address_, const_cast<uint8_t*>(urb.data()),
              static_cast<int>(urb.size()), &transferred, options_.timeout_ms);
        }
        if (rc == 0 && static_cast<size_t>(transferred) == urb.size()) break;
        if (rc == 0) rc = LIBUSB_ERROR_IO;
        if (!auto_mode_ || attempt == 1 ||
            (rc != LIBUSB_ERROR_INVALID_PARAM &&
             rc != LIBUSB_ERROR_NOT_SUPPORTED && rc != LIBUSB_ERROR_PIPE &&
             rc != LIBUSB_ERROR_TIMEOUT && rc != LIBUSB_ERROR_IO)) {
          break;
        }
        // 首次失败时改用另一种传输类型重试一次。
        mode_ = (mode_ == TransferMode::Bulk) ? TransferMode::Interrupt
                                              : TransferMode::Bulk;
        log::info(std::string("usb-video: retrying with ") + transfer_name() +
                " transfer");
      }
      if (rc != 0) {
        auto_mode_ = false;
        if (err) {
          *err = std::string("usb-video EP") +
                 std::to_string(endpoint_address_ & 0x7f) + " " +
                 transfer_name() + " write failed: " + libusb_strerror(rc);
        }
        return false;
      }
      ++packets_sent_;
    }
    ++frames_sent_;
    return true;
  }

  std::string describe() const override {
    char buf[96];
    std::snprintf(buf, sizeof(buf), "usb:%04x:%04x IF%d EP0x%02x %zuB",
                  options_.vid, options_.pid, options_.interface_number,
                  endpoint_address_, options_.urb_size);
    return buf;
  }
  uint64_t packets_sent() const override { return packets_sent_; }
  uint64_t frames_sent() const override { return frames_sent_; }

 private:
  enum class TransferMode { Auto = 0, Interrupt = 1, Bulk = 2 };
  const char* transfer_name() const {
    return mode_ == TransferMode::Interrupt ? "interrupt" : "bulk";
  }

  VideoUrbOptions options_;
  libusb_context* context_ = nullptr;
  libusb_device_handle* handle_ = nullptr;
  bool claimed_interface_ = false;
  bool detached_kernel_driver_ = false;
  uint8_t endpoint_address_ = 0;
  TransferMode mode_ = TransferMode::Auto;
  bool auto_mode_ = true;
  uint64_t packets_sent_ = 0;
  uint64_t frames_sent_ = 0;
};

class DumpVideoSink : public UsbVideoSink {
 public:
  DumpVideoSink(std::string path, const VideoUrbOptions& options)
      : path_(std::move(path)), options_(options) {}
  ~DumpVideoSink() override { close(); }

  bool open(std::string* err) override {
    if (!writer_.open(path_, options_.urb_size, err)) return false;
    log::info("usb-video: dumping URBs to " + path_);
    return true;
  }
  void close() override { writer_.close(); }
  bool connected() const override { return writer_.is_open(); }

  bool send_frame(const uint8_t* annexb, size_t len, uint32_t width,
                  uint32_t height, std::string* err) override {
    std::vector<std::vector<uint8_t>> urbs;
    if (!build_video_urbs(annexb, len, width, height, options_, &urbs, err)) {
      return false;
    }
    for (const auto& urb : urbs) {
      if (!writer_.write(urb.data(), urb.size(), err)) return false;
      ++packets_sent_;
    }
    ++frames_sent_;
    return true;
  }
  std::string describe() const override { return "usb-dump:" + path_; }
  uint64_t packets_sent() const override { return packets_sent_; }
  uint64_t frames_sent() const override { return frames_sent_; }

 private:
  std::string path_;
  VideoUrbOptions options_;
  hid::ReportDumpWriter writer_;
  uint64_t packets_sent_ = 0;
  uint64_t frames_sent_ = 0;
};

class NullVideoSink : public UsbVideoSink {
 public:
  explicit NullVideoSink(const VideoUrbOptions& options) : options_(options) {}
  bool open(std::string*) override {
    open_ = true;
    return true;
  }
  void close() override { open_ = false; }
  bool connected() const override { return open_; }
  bool send_frame(const uint8_t* annexb, size_t len, uint32_t width,
                  uint32_t height, std::string* err) override {
    std::vector<std::vector<uint8_t>> urbs;
    if (!build_video_urbs(annexb, len, width, height, options_, &urbs, err)) {
      return false;
    }
    packets_sent_ += urbs.size();
    ++frames_sent_;
    return true;
  }
  std::string describe() const override { return "usb-null"; }
  uint64_t packets_sent() const override { return packets_sent_; }
  uint64_t frames_sent() const override { return frames_sent_; }

 private:
  VideoUrbOptions options_;
  bool open_ = false;
  uint64_t packets_sent_ = 0;
  uint64_t frames_sent_ = 0;
};

}  // namespace

bool build_video_urbs(const uint8_t* annexb, size_t len, uint32_t width,
                      uint32_t height, const VideoUrbOptions& options,
                      std::vector<std::vector<uint8_t>>* out,
                      std::string* err) {
  out->clear();
  if (annexb == nullptr || len == 0) {
    if (err) *err = "empty video frame";
    return false;
  }
  if (options.urb_size <= kUrbHeaderSize) {
    if (err) *err = "URB size too small for the 16-byte header";
    return false;
  }
  const size_t payload_budget = options.urb_size - kUrbHeaderSize;
  size_t offset = 0;
  bool first = true;
  while (offset < len) {
    const size_t chunk = std::min(payload_budget, len - offset);
    std::vector<uint8_t> urb(options.urb_size, 0);
    urb[0] = options.report_id;
    if (first) {
      write_be32(urb.data() + 1, static_cast<uint32_t>(len));
      // 第 5 到 7 字节保持为零
      write_be32(urb.data() + 8, width);
      write_be32(urb.data() + 12, height);
      first = false;
    }
    std::memcpy(urb.data() + kUrbHeaderSize, annexb + offset, chunk);
    offset += chunk;
    out->push_back(std::move(urb));
  }
  return true;
}

std::unique_ptr<UsbVideoSink> make_libusb_video_sink(
    const VideoUrbOptions& options) {
  return std::unique_ptr<UsbVideoSink>(new LibusbVideoSink(options));
}

std::unique_ptr<UsbVideoSink> make_dump_video_sink(
    const std::string& path, const VideoUrbOptions& options) {
  return std::unique_ptr<UsbVideoSink>(new DumpVideoSink(path, options));
}

std::unique_ptr<UsbVideoSink> make_null_video_sink(
    const VideoUrbOptions& options) {
  return std::unique_ptr<UsbVideoSink>(new NullVideoSink(options));
}

}  // namespace transport
}  // namespace usbdisplay
