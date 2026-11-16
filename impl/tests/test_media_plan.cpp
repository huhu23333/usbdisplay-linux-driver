// SPDX-License-Identifier: MIT
#include <string>

#include "media_plan.h"
#include "test_harness.h"

namespace {

usbdisplay::VideoConfig make_video() {
  usbdisplay::VideoConfig video;
  video.width = 1920;
  video.height = 1080;
  video.fps = 60;
  video.bitrate_bps = 4000000;
  return video;
}

usbdisplay::AudioConfig make_audio() {
  usbdisplay::AudioConfig audio;
  audio.sample_rate = 48000;
  audio.channels = 2;
  audio.frame_ms = 20;
  return audio;
}

usbdisplay::VideoConfig empty_video() {
  usbdisplay::VideoConfig video;
  video.width = 0;
  video.height = 0;
  video.fps = 0;
  video.bitrate_bps = 0;
  return video;
}

usbdisplay::AudioConfig empty_audio() {
  usbdisplay::AudioConfig audio;
  audio.sample_rate = 0;
  audio.channels = 0;
  audio.frame_ms = 0;
  return audio;
}

}  // namespace

int main() {
  using namespace usbdisplay;
  std::string err;
  {
    // 纯音频配置合法。
    CHECK(validate_media_config(empty_video(), make_audio(), &err));
  }
  {
    // 纯视频配置合法。
    CHECK(validate_media_config(make_video(), empty_audio(), &err));
  }
  {
    // 音视频同时配置合法。
    CHECK(validate_media_config(make_video(), make_audio(), &err));
  }
  {
    // 两侧都为空时拒绝。
    CHECK(!validate_media_config(empty_video(), empty_audio(), &err));
  }
  {
    // 视频参数不完整时拒绝。
    VideoConfig video = make_video();
    video.height = 0;
    CHECK(!validate_media_config(video, make_audio(), &err));
  }
  {
    // 音频参数不完整时拒绝。
    AudioConfig audio = make_audio();
    audio.channels = 0;
    CHECK(!validate_media_config(empty_video(), audio, &err));
  }
  {
    // selector 状态：纯音频模式不启用视频。
    DeviceSelector selector;
    selector.enable_audio = true;
    selector.disable_video = true;
    MediaPlan plan = media_plan_from_selector(selector);
    CHECK(plan.has_audio);
    CHECK(!plan.has_video);
  }
  {
    // selector 状态：纯视频模式不启用音频。
    DeviceSelector selector;
    selector.enable_audio = false;
    selector.disable_video = false;
    MediaPlan plan = media_plan_from_selector(selector);
    CHECK(!plan.has_audio);
    CHECK(plan.has_video);
  }
  return testharness::finish("test_media_plan");
}
