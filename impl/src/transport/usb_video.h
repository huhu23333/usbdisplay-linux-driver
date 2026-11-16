// SPDX-License-Identifier: MIT
// USB 视频通道：通过 libusb 向接口 1 的端点 2 输出方向发送 4096 字节 URB。
//
// 一帧数据由一条或多条 URB 组成。首条 URB 的头部占 16 字节：
// [0] 报告编号 0x02
// [1..4] 大端序，整帧 H.264 数据长度，包含起始码
// [5..7] 保留，取值为零
// [8..11] 大端序宽度
// [12..15] 大端序高度
// [16..] 整帧数据的前 4080 字节，不足部分补零
// 后续 URB 的头 16 字节全部为零，[16..] 为后续 4080 字节数据。
//
// 使用端点 2 前需要解除内核 HID 驱动对接口 1 的绑定，libusb 会在句柄关闭后重新绑定。
// 访问 /dev/bus/usb 需要 root 权限或 udev 规则。
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
  // 取值为零表示自动发现接口上的输出端点。
  uint8_t endpoint_address = 0;
  size_t urb_size = 4096;
  uint8_t report_id = 0x02;
  int timeout_ms = 2000;
  // auto 先尝试 interrupt，失败后改用 bulk。
  std::string transfer_mode = "auto";  // auto|interrupt|bulk
};

// 把一个 Annex-B 访问单元拆分为 4096 字节 URB，输入数据原样复制，
// 起始码必须已经存在。
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

// 真实设备通道：libusb 端点 2 输出方向，解除绑定后声明接口 1。
std::unique_ptr<UsbVideoSink> make_libusb_video_sink(
    const VideoUrbOptions& options);

// 离线通道：把全部 URB 写入转储文件，供 fake_device 校验。
std::unique_ptr<UsbVideoSink> make_dump_video_sink(
    const std::string& path, const VideoUrbOptions& options);

// 空通道：不写入任何数据，只统计帧数与报告数。
std::unique_ptr<UsbVideoSink> make_null_video_sink(
    const VideoUrbOptions& options);

}  // namespace transport
}  // namespace usbdisplay
