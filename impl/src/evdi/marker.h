// SPDX-License-Identifier: MIT
// /run/usbdisplay/evdi-card marker 解析与所有权校验。
#pragma once

#include <string>

namespace usbdisplay {
namespace evdi {

struct Marker {
  bool valid = false;
  std::string error;
  std::string card;
  std::string card_name;
  std::string connector;
  std::string owner;
  std::string platform;
  int width = 0;
  int height = 0;
  int card_index = -1;
};

// 解析 key=value 文本；只接受 owner=usbdisplay-linux-driver 与 1920x1080。
Marker parse_marker(const std::string& text);

// 读取 marker 文件；文件不存在或不可读时返回无效 marker。
Marker load_marker(const std::string& path);

}  // namespace evdi
}  // namespace usbdisplay
