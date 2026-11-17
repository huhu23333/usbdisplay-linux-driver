// SPDX-License-Identifier: MIT
#include "evdi/marker.h"

#include <cctype>
#include <cstdlib>
#include <fstream>
#include <map>
#include <sstream>

namespace usbdisplay {
namespace evdi {
namespace {

std::string trim(const std::string& value) {
  size_t begin = 0;
  size_t end = value.size();
  while (begin < end && std::isspace(static_cast<unsigned char>(value[begin]))) {
    ++begin;
  }
  while (end > begin &&
         std::isspace(static_cast<unsigned char>(value[end - 1]))) {
    --end;
  }
  return value.substr(begin, end - begin);
}

bool parse_int(const std::string& value, int* out) {
  if (value.empty()) return false;
  char* end = nullptr;
  const long parsed = std::strtol(value.c_str(), &end, 10);
  if (end == nullptr || *end != '\0' || parsed < 0 || parsed > 4096) {
    return false;
  }
  *out = static_cast<int>(parsed);
  return true;
}

}  // namespace

Marker parse_marker(const std::string& text) {
  Marker marker;
  std::map<std::string, std::string> values;
  std::istringstream lines(text);
  std::string line;
  while (std::getline(lines, line)) {
    line = trim(line);
    if (line.empty() || line[0] == '#') continue;
    const size_t equals = line.find('=');
    if (equals == std::string::npos) {
      marker.error = "marker 行缺少等号：" + line;
      return marker;
    }
    values[trim(line.substr(0, equals))] = trim(line.substr(equals + 1));
  }

  marker.card = values["card"];
  marker.card_name = values["card_name"];
  marker.connector = values["connector"];
  marker.owner = values["owner"];
  marker.platform = values["platform"];

  if (marker.owner != "usbdisplay-linux-driver") {
    marker.error = "marker owner 不是 usbdisplay-linux-driver";
    return marker;
  }
  const std::string prefix = "/dev/dri/card";
  if (marker.card.compare(0, prefix.size(), prefix) != 0) {
    marker.error = "marker card 路径无效";
    return marker;
  }
  const std::string index_text = marker.card.substr(prefix.size());
  if (!parse_int(index_text, &marker.card_index)) {
    marker.error = "marker card 编号无效";
    return marker;
  }
  if (!marker.card_name.empty() && marker.card_name != "card" + index_text) {
    marker.error = "marker card_name 与 card 不一致";
    return marker;
  }
  if (marker.card_name.empty()) marker.card_name = "card" + index_text;
  if (marker.connector.empty()) {
    marker.error = "marker connector 为空";
    return marker;
  }
  if (!parse_int(values["width"], &marker.width) ||
      !parse_int(values["height"], &marker.height)) {
    marker.error = "marker width/height 无效";
    return marker;
  }
  if (marker.width != 1920 || marker.height != 1080) {
    marker.error = "marker 分辨率不是 1920x1080";
    return marker;
  }
  marker.valid = true;
  return marker;
}

Marker load_marker(const std::string& path) {
  std::ifstream input(path);
  if (!input) {
    Marker marker;
    marker.error = "无法读取 marker：" + path;
    return marker;
  }
  std::ostringstream buffer;
  buffer << input.rdbuf();
  return parse_marker(buffer.str());
}

}  // namespace evdi
}  // namespace usbdisplay
