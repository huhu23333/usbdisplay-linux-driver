// SPDX-License-Identifier: MIT
// 音视频配置校验与选择状态，便于分别验证纯视频、纯音频和音视频模式。
#pragma once

#include <string>

#include "usbdisplay/types.h"

namespace usbdisplay {

struct MediaPlan {
  bool has_video = false;
  bool has_audio = false;
};

bool validate_media_config(const VideoConfig& video, const AudioConfig& audio,
                           std::string* err);

MediaPlan media_plan_from_selector(const DeviceSelector& selector);

}  // namespace usbdisplay
