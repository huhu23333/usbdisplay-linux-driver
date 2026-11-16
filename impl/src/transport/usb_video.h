// SPDX-License-Identifier: MIT
// Verified USB Display video transport: libusb interrupt/bulk writes to IF1
// EP2 OUT using exactly 4096-byte URBs (NOT the 12288-byte HID report length
// from the report descriptor).
//
// Verified message layout (USB_PROTOCOL_VERIFIED.md section 3, 1.pcapng):
//   first URB of a frame:
//     [0]      = report id (0x02)
//     [1..4]   = u32 BE total H.264 frame length L
//     [5..7]   = 0x00 x3
//     [8..11]  = u32 BE width
//     [12..15] = u32 BE height
//     [16..]   = first 4080 bytes of the Annex-B frame (zero padded to 4096)
//   continuation URBs (when L > 4080):
//     [0]      = 0x02
//     [1..15]  = 0x00 x15
//     [16..]   = next 4080 bytes of Annex-B data (zero padded)
//
// To use EP2 from userspace the kernel HID driver must be detached from
// interface 1 (libusb does this; the kernel re-attaches when the handle is
// closed). Root / udev rules are required for /dev/bus/usb access.
#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace usbdisplay {
namespace transport {

struct VideoUrbOptions {
  uint16_t vid = 0x1DE1;
  uint16_t pid = 0xF201;
  int interface_number = 1;
  // 0 = auto-discover the OUT endpoint on the interface.
  uint8_t endpoint_address = 0;
  size_t urb_size = 4096;
  uint8_t report_id = 0x02;
  int timeout_ms = 2000;
  // auto tries interrupt first and falls back to bulk (the Windows app's
  // pyusb reference implementation uses a bulk write on the interrupt EP).
  std::string transfer_mode = "auto";  // auto|interrupt|bulk
};

// Split one Annex-B access unit into 4096-byte URBs. The input is copied
// verbatim (start codes must already be present).
bool build_video_urbs(const uint8_t* annexb, size_t len, uint32_t width,
                      uint32_t height, const VideoUrbOptions& options,
                      std::vector<std::vector<uint8_t>>* out, std::string* err);

class UsbVideoSink {
 public:
  virtual ~UsbVideoSink() = default;
  virtual bool open(std::string* err) = 0;
  virtual void close() = 0;
  virtual bool connected() const = 0;
  virtual bool send_frame(const uint8_t* annexb, size_t len, uint32_t width,
                          uint32_t height, std::string* err) = 0;
  virtual std::string describe() const = 0;
  virtual uint64_t packets_sent() const = 0;
  virtual uint64_t frames_sent() const = 0;
};

// Real device path (libusb EP2 OUT, detach/claim interface 1).
std::unique_ptr<UsbVideoSink> make_libusb_video_sink(
    const VideoUrbOptions& options);

// Offline path: write the exact URBs to a USBDRPT1 dump for fake_device.
std::unique_ptr<UsbVideoSink> make_dump_video_sink(
    const std::string& path, const VideoUrbOptions& options);

// No-I/O sink for dry runs without a dump file (counts frames/reports only).
std::unique_ptr<UsbVideoSink> make_null_video_sink(
    const VideoUrbOptions& options);

}  // namespace transport
}  // namespace usbdisplay
