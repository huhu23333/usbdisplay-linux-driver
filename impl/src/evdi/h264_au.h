// SPDX-License-Identifier: MIT
// x264 输出按访问单元聚合，保留 AUD 与 Annex-B 起始码。
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace usbdisplay {
namespace evdi {

struct H264NalView {
  const uint8_t* data = nullptr;
  size_t size = 0;
};

struct H264AccessUnit {
  std::vector<uint8_t> data;
  bool keyframe = false;
};

// nals 来自一次 x264_encoder_encode 调用；keyframe 来自 pic_out.b_keyframe。
bool build_access_unit(const std::vector<H264NalView>& nals, bool keyframe,
                       H264AccessUnit* out, std::string* err);

}  // namespace evdi
}  // namespace usbdisplay
