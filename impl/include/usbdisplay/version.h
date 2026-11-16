// SPDX-License-Identifier: MIT
// USB Display 接收器的 Linux 用户态发送库。
#pragma once

#define USBDISPLAY_VERSION_MAJOR 0
#define USBDISPLAY_VERSION_MINOR 1
#define USBDISPLAY_VERSION_PATCH 0
#define USBDISPLAY_VERSION_STRING "0.1.0"

namespace usbdisplay {
// 单行版本字符串，例如 "usbdisplay 0.1.0"。
const char* version_string();
}  // namespace usbdisplay
