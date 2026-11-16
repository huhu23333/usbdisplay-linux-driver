// SPDX-License-Identifier: MIT
// USB 设备复位与重新枚举工具。
//
// 设备固件只接受重新枚举后的会话，因此调用 libusb_reset_device 后重新发现设备，
// 无需人工拔插。
#pragma once

#include <cstdint>
#include <string>

namespace usbdisplay {
namespace transport {

// 打开设备但不声明接口，调用 libusb_reset_device 后关闭。
// 调用前必须关闭本进程内的 hidraw 句柄与已声明的接口。
bool usb_reset_device(uint16_t vid, uint16_t pid, std::string* err);

// 等待复位后的设备重新出现，期间设备可能短暂消失并以新的 USB 地址重新枚举。
bool wait_for_usb_device(uint16_t vid, uint16_t pid, int timeout_ms,
                         std::string* err);

}  // namespace transport
}  // namespace usbdisplay
