// SPDX-License-Identifier: MIT
#include "media_plan.h"

namespace usbdisplay {

bool validate_media_config(const VideoConfig& video, const AudioConfig& audio,
                           std::string* err) {
  const bool has_video = video.width != 0 || video.height != 0 ||
                         video.fps != 0 || video.bitrate_bps != 0;
  const bool has_audio = audio.sample_rate != 0 || audio.channels != 0 ||
                         audio.frame_ms != 0;
  if (!has_video && !has_audio) {
    if (err) *err = "no video or audio configured";
    return false;
  }
  if (has_video && (video.width == 0 || video.height == 0 || video.fps == 0 ||
                    video.bitrate_bps == 0)) {
    if (err) *err = "invalid video config";
    return false;
  }
  if (has_audio &&
      (audio.sample_rate == 0 || audio.channels == 0 || audio.channels > 8 ||
       audio.frame_ms == 0 || audio.frame_ms > 1000)) {
    if (err) *err = "invalid audio config";
    return false;
  }
  return true;
}

MediaPlan media_plan_from_selector(const DeviceSelector& selector) {
  MediaPlan plan;
  plan.has_video = !selector.disable_video;
  plan.has_audio = selector.enable_audio;
  return plan;
}

}  // namespace usbdisplay
