// SPDX-License-Identifier: MIT
#include "evdi/h264_au.h"

namespace usbdisplay {
namespace evdi {
namespace {

bool has_start_code(const uint8_t* data, size_t size) {
  if (size >= 4 && data[0] == 0 && data[1] == 0 && data[2] == 0 &&
      data[3] == 1) {
    return true;
  }
  return size >= 3 && data[0] == 0 && data[1] == 0 && data[2] == 1;
}

void append_start_code(std::vector<uint8_t>* out) {
  out->push_back(0);
  out->push_back(0);
  out->push_back(0);
  out->push_back(1);
}

}  // namespace

bool build_access_unit(const std::vector<H264NalView>& nals, bool keyframe,
                       H264AccessUnit* out, std::string* err) {
  if (out == nullptr) {
    if (err) *err = "访问单元输出为空";
    return false;
  }
  out->data.clear();
  out->keyframe = keyframe;
  if (nals.empty()) {
    if (err) *err = "没有 NAL 数据";
    return false;
  }
  for (const auto& nal : nals) {
    if (nal.data == nullptr || nal.size == 0) {
      if (err) *err = "NAL 数据为空";
      return false;
    }
    if (!has_start_code(nal.data, nal.size)) append_start_code(&out->data);
    out->data.insert(out->data.end(), nal.data, nal.data + nal.size);
  }
  return true;
}

}  // namespace evdi
}  // namespace usbdisplay
