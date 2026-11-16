// SPDX-License-Identifier: MIT
// 临时释放 USB 声卡：播放前把 PulseAudio 声卡 profile 设为 off，退出前恢复。
#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace usbdisplay {
namespace audio {

struct PulseCardInfo {
  uint32_t index = 0;
  std::string name;
  std::string description;
  std::string active_profile;
};

class PulseCardControl {
 public:
  virtual ~PulseCardControl() = default;
  virtual bool list_cards(std::vector<PulseCardInfo>* out, std::string* err) = 0;
  virtual bool set_card_profile(uint32_t index, const std::string& profile,
                                std::string* err) = 0;
};

std::unique_ptr<PulseCardControl> make_libpulse_card_control();

// 判断声卡列表中是否存在 USB 显示设备声卡。
bool has_usb_card(const std::vector<PulseCardInfo>& cards);

class UsbCardProfileGuard {
 public:
  explicit UsbCardProfileGuard(PulseCardControl* control,
                               std::string off_profile = "off");
  ~UsbCardProfileGuard();

  // 查找 USB 声卡，保存当前 profile 并设为 off。
  bool disable(std::string* err);
  // 恢复保存的 profile。
  bool restore(std::string* err);

  bool disabled() const;
  bool changed() const;
  const PulseCardInfo& card() const;

 private:
  PulseCardControl* control_;
  std::string off_profile_;
  PulseCardInfo card_;
  std::string saved_profile_;
  bool disabled_ = false;
  bool changed_ = false;
};

}  // namespace audio
}  // namespace usbdisplay
