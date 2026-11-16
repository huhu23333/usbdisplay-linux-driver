// SPDX-License-Identifier: MIT
// usbdisplay - Linux userspace sender for Actions Micro "USB Display" (EZMira) receivers.
#pragma once

#define USBDISPLAY_VERSION_MAJOR 0
#define USBDISPLAY_VERSION_MINOR 1
#define USBDISPLAY_VERSION_PATCH 0
#define USBDISPLAY_VERSION_STRING "0.1.0"

namespace usbdisplay {
// Human readable one-line version, e.g. "usbdisplay 0.1.0".
const char* version_string();
}  // namespace usbdisplay
