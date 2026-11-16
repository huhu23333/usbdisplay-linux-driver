// SPDX-License-Identifier: MIT
#include <string>
#include <vector>

#include "audio/pulse_card.h"
#include "test_harness.h"

namespace {

class FakeCardControl : public usbdisplay::audio::PulseCardControl {
 public:
  std::vector<usbdisplay::audio::PulseCardInfo> cards;
  int list_calls = 0;
  int set_calls = 0;
  bool fail_set = false;
  uint32_t last_index = 0;
  std::string last_profile;

  bool list_cards(std::vector<usbdisplay::audio::PulseCardInfo>* out,
                  std::string*) override {
    ++list_calls;
    *out = cards;
    return true;
  }
  bool set_card_profile(uint32_t index, const std::string& profile,
                        std::string* err) override {
    ++set_calls;
    last_index = index;
    last_profile = profile;
    if (fail_set) {
      if (err) *err = "profile 设置失败";
      return false;
    }
    return true;
  }
};

usbdisplay::audio::PulseCardInfo make_card(
    const std::string& name, const std::string& profile, uint32_t index = 7) {
  usbdisplay::audio::PulseCardInfo card;
  card.index = index;
  card.name = name;
  card.active_profile = profile;
  return card;
}

}  // namespace

int main() {
  using namespace usbdisplay::audio;
  const std::string usb_card =
      "alsa_card.usb-Actions_USB_DISPLAY_51d3f01-02";
  {
    // 保存当前 profile，设为 off，退出时恢复原 profile。
    FakeCardControl control;
    control.cards = {make_card(usb_card, "analog-stereo")};
    UsbCardProfileGuard guard(&control);
    std::string err;
    CHECK(guard.disable(&err));
    CHECK(control.set_calls == 1);
    CHECK(control.last_index == 7);
    CHECK(control.last_profile == "off");
    CHECK(guard.disabled());
    CHECK(guard.changed());
    CHECK(guard.card().active_profile == "analog-stereo");
    CHECK(guard.restore(&err));
    CHECK(control.set_calls == 2);
    CHECK(control.last_profile == "analog-stereo");
    CHECK(!guard.disabled());
    CHECK(!guard.changed());
  }
  {
    // 找不到 USB 声卡时不修改任何 profile。
    FakeCardControl control;
    control.cards = {make_card("alsa_card.pci-0000_00_1f.3", "analog-stereo")};
    UsbCardProfileGuard guard(&control);
    std::string err;
    CHECK(!guard.disable(&err));
    CHECK(err.find("未找到") != std::string::npos);
    CHECK(control.set_calls == 0);
    CHECK(guard.restore(&err));
  }
  {
    // 已经是 off 时不重复设置，恢复也不修改。
    FakeCardControl control;
    control.cards = {make_card(usb_card, "off")};
    UsbCardProfileGuard guard(&control);
    std::string err;
    CHECK(guard.disable(&err));
    CHECK(control.set_calls == 0);
    CHECK(guard.disabled());
    CHECK(!guard.changed());
    CHECK(guard.restore(&err));
    CHECK(control.set_calls == 0);
  }
  {
    // 关闭 profile 失败时返回错误且不处于已释放状态。
    FakeCardControl control;
    control.cards = {make_card(usb_card, "analog-stereo")};
    control.fail_set = true;
    UsbCardProfileGuard guard(&control);
    std::string err;
    CHECK(!guard.disable(&err));
    CHECK(err.find("关闭") != std::string::npos);
    CHECK(!guard.disabled());
    CHECK(!guard.changed());
  }
  {
    // 恢复失败时返回错误，便于调用方记录警告。
    FakeCardControl control;
    control.cards = {make_card(usb_card, "analog-stereo")};
    UsbCardProfileGuard guard(&control);
    std::string err;
    CHECK(guard.disable(&err));
    control.fail_set = true;
    CHECK(!guard.restore(&err));
    CHECK(err.find("恢复") != std::string::npos);
    CHECK(guard.disabled());
    CHECK(guard.changed());
  }
  {
    // 匹配描述中的 USBDISPLAY。
    FakeCardControl control;
    PulseCardInfo card = make_card("alsa_card.usb-1de1", "analog-stereo");
    card.description = "USB_DISPLAY audio";
    control.cards = {card};
    UsbCardProfileGuard guard(&control);
    std::string err;
    CHECK(guard.disable(&err));
    CHECK(control.set_calls == 1);
  }
  return testharness::finish("test_pulse_card");
}
