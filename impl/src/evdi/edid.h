// SPDX-License-Identifier: MIT
// 生成 1920x1080@60/120 双 DTD、显示器名 USBDisplay 的 EDID。
#pragma once
#include <cstdint>
#include <vector>
namespace usbdisplay { namespace evdi {
// 128 字节基础 EDID，同时包含 1920x1080@60 与 @120 DTD、monitor name 与 range。
std::vector<uint8_t> make_edid_1920x1080();
std::vector<uint8_t> make_edid_1920x1080_60();
std::vector<uint8_t> make_edid_1920x1080_120();
}}  // namespace
