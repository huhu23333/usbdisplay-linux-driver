// SPDX-License-Identifier: MIT
#include <string>

#include "evdi/marker.h"
#include "test_harness.h"

namespace {

const char* kValid =
    "card=/dev/dri/card5\n"
    "card_name=card5\n"
    "connector=DVI-I-5\n"
    "width=1920\n"
    "height=1080\n"
    "owner=usbdisplay-linux-driver\n"
    "platform=/sys/devices/platform/evdi.1\n";

}  // namespace

int main() {
  using namespace usbdisplay::evdi;
  {
    Marker marker = parse_marker(kValid);
    CHECK(marker.valid);
    CHECK(marker.card == "/dev/dri/card5");
    CHECK(marker.card_name == "card5");
    CHECK(marker.connector == "DVI-I-5");
    CHECK(marker.owner == "usbdisplay-linux-driver");
    CHECK(marker.width == 1920 && marker.height == 1080);
    CHECK(marker.card_index == 5);
  }
  {
    // card0 是合法编号。
    Marker marker = parse_marker(
        "card=/dev/dri/card0\nconnector=DVI-I-1\nwidth=1920\n"
        "height=1080\nowner=usbdisplay-linux-driver\n");
    CHECK(marker.valid);
    CHECK(marker.card_index == 0);
    CHECK(marker.card_name == "card0");
  }
  {
    // owner 不匹配时拒绝。
    std::string text = kValid;
    const std::string owner = "owner=usbdisplay-linux-driver";
    text.replace(text.find(owner), owner.size(), "owner=other-driver");
    Marker marker = parse_marker(text);
    CHECK(!marker.valid);
    CHECK(marker.error.find("owner") != std::string::npos);
  }
  {
    // 非 1920x1080 时拒绝。
    std::string text = kValid;
    const std::string height = "height=1080";
    text.replace(text.find(height), height.size(), "height=720");
    Marker marker = parse_marker(text);
    CHECK(!marker.valid);
    CHECK(marker.error.find("1920x1080") != std::string::npos);
  }
  {
    // card 路径与 card_name 不一致时拒绝。
    std::string text = kValid;
    const std::string name = "card_name=card5";
    text.replace(text.find(name), name.size(), "card_name=card4");
    Marker marker = parse_marker(text);
    CHECK(!marker.valid);
  }
  {
    // 缺少字段、行格式错误均拒绝。
    CHECK(!parse_marker("card=/dev/dri/card5\nowner=usbdisplay-linux-driver\n").valid);
    CHECK(!parse_marker("owner=usbdisplay-linux-driver\nbad-line\n").valid);
  }
  {
    Marker missing = load_marker("/tmp/usbdisplay-no-such-marker");
    CHECK(!missing.valid);
    CHECK(!missing.error.empty());
  }
  return testharness::finish("test_evdi_marker");
}
