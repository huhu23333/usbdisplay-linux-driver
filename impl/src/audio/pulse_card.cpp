// SPDX-License-Identifier: MIT
#include "audio/pulse_card.h"

#include <algorithm>
#include <cctype>

#include "common/log.h"
#include "common/util.h"

#ifdef USBDISPLAY_HAVE_PULSE
#include <pulse/pulseaudio.h>
#endif

namespace usbdisplay {
namespace audio {
namespace {

std::string lower(std::string text) {
  std::transform(text.begin(), text.end(), text.begin(),
                 [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return text;
}

bool card_matches(const PulseCardInfo& card) {
  const std::string text = lower(card.name + " " + card.description);
  return text.find("usbdisplay") != std::string::npos ||
         text.find("usb_display") != std::string::npos ||
         text.find("actions_usb_display") != std::string::npos ||
         text.find("usb-actions") != std::string::npos ||
         text.find("1de1") != std::string::npos ||
         text.find("f201") != std::string::npos;
}

#ifdef USBDISPLAY_HAVE_PULSE

struct CardQuery {
  bool done = false;
  bool ok = false;
  std::string error;
  std::vector<PulseCardInfo> cards;
};

void card_info_callback(pa_context*, const pa_card_info* info, int eol,
                        void* data) {
  CardQuery* query = static_cast<CardQuery*>(data);
  if (eol < 0) {
    query->done = true;
    query->ok = false;
    query->error = "查询声卡列表失败";
    return;
  }
  if (eol > 0) {
    query->done = true;
    query->ok = true;
    return;
  }
  PulseCardInfo card;
  card.index = info->index;
  if (info->name != nullptr) card.name = info->name;
  if (info->proplist != nullptr) {
    const char* description =
        pa_proplist_gets(info->proplist, PA_PROP_DEVICE_DESCRIPTION);
    if (description != nullptr) card.description = description;
  }
  if (info->active_profile != nullptr && info->active_profile->name != nullptr) {
    card.active_profile = info->active_profile->name;
  }
  query->cards.push_back(std::move(card));
}

void context_state_callback(pa_context* context, void* data) {
  CardQuery* query = static_cast<CardQuery*>(data);
  switch (pa_context_get_state(context)) {
    case PA_CONTEXT_READY:
      pa_context_get_card_info_list(context, card_info_callback, query);
      break;
    case PA_CONTEXT_FAILED:
    case PA_CONTEXT_TERMINATED:
      query->done = true;
      query->ok = false;
      if (query->error.empty()) query->error = "PulseAudio 连接失败";
      break;
    default:
      break;
  }
}

bool run_card_query(CardQuery* query) {
  pa_mainloop* mainloop = pa_mainloop_new();
  if (mainloop == nullptr) {
    query->error = "创建 PulseAudio 主循环失败";
    return false;
  }
  pa_mainloop_api* api = pa_mainloop_get_api(mainloop);
  pa_context* context = pa_context_new(api, "usbdisplay");
  if (context == nullptr) {
    pa_mainloop_free(mainloop);
    query->error = "创建 PulseAudio 上下文失败";
    return false;
  }
  pa_context_set_state_callback(context, context_state_callback, query);
  if (pa_context_connect(context, nullptr, PA_CONTEXT_NOFLAGS, nullptr) < 0) {
    query->error = "连接 PulseAudio 失败";
    pa_context_unref(context);
    pa_mainloop_free(mainloop);
    return false;
  }
  const uint64_t deadline = mono_ms() + 3000;
  while (!query->done && mono_ms() < deadline) {
    if (pa_mainloop_prepare(mainloop, 100000) < 0) break;
    if (pa_mainloop_poll(mainloop) < 0) break;
    if (pa_mainloop_dispatch(mainloop) < 0) break;
  }
  const bool completed = query->done;
  const bool success = query->ok;
  pa_context_disconnect(context);
  pa_context_unref(context);
  pa_mainloop_free(mainloop);
  if (!completed) {
    query->error = "查询声卡超时";
    return false;
  }
  return success;
}

class LibpulseCardControl : public PulseCardControl {
 public:
  bool list_cards(std::vector<PulseCardInfo>* out, std::string* err) override {
    CardQuery query;
    if (!run_card_query(&query)) {
      if (err) *err = query.error;
      return false;
    }
    *out = std::move(query.cards);
    return true;
  }

  bool set_card_profile(uint32_t index, const std::string& profile,
                        std::string* err) override {
    struct SetState {
      uint32_t index = 0;
      std::string profile;
      bool done = false;
      bool ok = false;
      std::string error;
    } state;
    state.index = index;
    state.profile = profile;
    pa_mainloop* mainloop = pa_mainloop_new();
    if (mainloop == nullptr) {
      if (err) *err = "创建 PulseAudio 主循环失败";
      return false;
    }
    pa_mainloop_api* api = pa_mainloop_get_api(mainloop);
    pa_context* context = pa_context_new(api, "usbdisplay");
    if (context == nullptr) {
      pa_mainloop_free(mainloop);
      if (err) *err = "创建 PulseAudio 上下文失败";
      return false;
    }
    pa_context_set_state_callback(
        context,
        [](pa_context* ctx, void* data) {
          SetState* set = static_cast<SetState*>(data);
          if (pa_context_get_state(ctx) == PA_CONTEXT_READY) {
            pa_context_set_card_profile_by_index(
                ctx, set->index, set->profile.c_str(),
                [](pa_context*, int success, void* result_data) {
                  SetState* result = static_cast<SetState*>(result_data);
                  result->done = true;
                  result->ok = success != 0;
                },
                set);
          } else if (pa_context_get_state(ctx) == PA_CONTEXT_FAILED ||
                     pa_context_get_state(ctx) == PA_CONTEXT_TERMINATED) {
            set->done = true;
            set->ok = false;
          }
        },
        &state);
    if (pa_context_connect(context, nullptr, PA_CONTEXT_NOFLAGS, nullptr) < 0) {
      if (err) *err = "连接 PulseAudio 失败";
      pa_context_unref(context);
      pa_mainloop_free(mainloop);
      return false;
    }
    const uint64_t deadline = mono_ms() + 3000;
    while (!state.done && mono_ms() < deadline) {
      if (pa_mainloop_prepare(mainloop, 100000) < 0) break;
      if (pa_mainloop_poll(mainloop) < 0) break;
      if (pa_mainloop_dispatch(mainloop) < 0) break;
    }
    const bool completed = state.done;
    const bool success = state.ok;
    pa_context_disconnect(context);
    pa_context_unref(context);
    pa_mainloop_free(mainloop);
    if (!completed) {
      if (err) *err = "设置声卡 profile 超时";
      return false;
    }
    if (!success) {
      if (err) *err = "设置声卡 profile 失败";
      return false;
    }
    return true;
  }
};

#else  // !USBDISPLAY_HAVE_PULSE

class LibpulseCardControl : public PulseCardControl {
 public:
  bool list_cards(std::vector<PulseCardInfo>*, std::string* err) override {
    if (err) *err = "构建时未启用 PulseAudio 支持";
    return false;
  }
  bool set_card_profile(uint32_t, const std::string&, std::string* err) override {
    if (err) *err = "构建时未启用 PulseAudio 支持";
    return false;
  }
};

#endif

}  // namespace

bool has_usb_card(const std::vector<PulseCardInfo>& cards) {
  for (const auto& card : cards) {
    if (card_matches(card)) return true;
  }
  return false;
}

UsbCardProfileGuard::UsbCardProfileGuard(PulseCardControl* control,
                                         std::string off_profile)
    : control_(control), off_profile_(std::move(off_profile)) {}

UsbCardProfileGuard::~UsbCardProfileGuard() {
  if (!disabled_) return;
  std::string err;
  if (!restore(&err)) {
    log::warn("恢复 USB 声卡 profile 失败：" + err);
  }
}

bool UsbCardProfileGuard::disable(std::string* err) {
  disabled_ = false;
  changed_ = false;
  card_ = PulseCardInfo();
  saved_profile_.clear();
  if (control_ == nullptr) {
    if (err) *err = "声卡控制接口为空";
    return false;
  }
  std::vector<PulseCardInfo> cards;
  std::string list_err;
  if (!control_->list_cards(&cards, &list_err)) {
    if (err) *err = list_err;
    return false;
  }
  for (const auto& card : cards) {
    if (card_matches(card)) {
      card_ = card;
      break;
    }
  }
  if (card_.name.empty()) {
    if (err) *err = "未找到 USB 显示设备的 PulseAudio 声卡";
    return false;
  }
  saved_profile_ = card_.active_profile;
  if (saved_profile_.empty()) {
    if (err) *err = "USB 声卡当前 profile 未知";
    return false;
  }
  disabled_ = true;
  if (saved_profile_ == off_profile_) return true;
  std::string set_err;
  if (!control_->set_card_profile(card_.index, off_profile_, &set_err)) {
    disabled_ = false;
    if (err) *err = "关闭 USB 声卡 profile 失败：" + set_err;
    return false;
  }
  changed_ = true;
  return true;
}

bool UsbCardProfileGuard::restore(std::string* err) {
  if (!disabled_) return true;
  if (changed_) {
    std::string set_err;
    if (!control_->set_card_profile(card_.index, saved_profile_, &set_err)) {
      if (err) *err = "恢复 USB 声卡 profile 失败：" + set_err;
      return false;
    }
    changed_ = false;
  }
  disabled_ = false;
  return true;
}

bool UsbCardProfileGuard::disabled() const { return disabled_; }
bool UsbCardProfileGuard::changed() const { return changed_; }
const PulseCardInfo& UsbCardProfileGuard::card() const { return card_; }

std::unique_ptr<PulseCardControl> make_libpulse_card_control() {
  return std::unique_ptr<PulseCardControl>(new LibpulseCardControl());
}

}  // namespace audio
}  // namespace usbdisplay
