// SPDX-License-Identifier: MIT
// 使用 libx264 编码 XRGB8888 帧，输出 Annex-B 访问单元。
#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace usbdisplay {
namespace evdi {

struct EncodedAccessUnit {
  std::vector<uint8_t> data;
  bool keyframe = false;
};

class X264Encoder {
 public:
  X264Encoder();
  ~X264Encoder();
  X264Encoder(const X264Encoder&) = delete;
  X264Encoder& operator=(const X264Encoder&) = delete;

  bool open(int width, int height, int fps, int bitrate_bps, std::string* err);
  bool encode(const uint8_t* xrgb, int stride, uint64_t pts_us,
              EncodedAccessUnit* out, std::string* err);
  void close();
  bool is_open() const;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace evdi
}  // namespace usbdisplay
