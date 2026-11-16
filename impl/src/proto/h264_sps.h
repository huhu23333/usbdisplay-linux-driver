// SPDX-License-Identifier: MIT
// 精简的 H.264 SPS 解析器，从 VUI 时间参数推算实际帧率：
// 帧率等于 time_scale 除以二倍的 num_units_in_tick。
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace usbdisplay {
namespace proto {
namespace h264 {

struct SpsInfo {
  uint8_t profile_idc = 0;
  uint8_t level_idc = 0;
  uint32_t width = 0;
  uint32_t height = 0;
  bool vui_present = false;
  bool timing_present = false;
  uint32_t num_units_in_tick = 0;
  uint32_t time_scale = 0;
  bool fixed_frame_rate = false;
  // 存在时间参数时由 time_scale 与 num_units_in_tick 计算得到。
  double fps = 0.0;
};

// nal 指向 NAL 头字节且长度包含该字节，解析时处理档次相关字段与防竞争字节。
bool parse_sps(const uint8_t* nal, size_t len, SpsInfo* out, std::string* err);

// 在 Annex-B 码流中查找首个 SPS 并返回其 VUI 帧率，找不到时返回假。
bool extract_fps_from_annexb(const uint8_t* data, size_t len, double* fps,
                             SpsInfo* info, std::string* err);

// 生成用于测试与诊断的 SPS 数据，不包含起始码。
std::vector<uint8_t> make_test_sps(uint32_t width, uint32_t height,
                                   uint32_t num_units_in_tick,
                                   uint32_t time_scale, bool with_vui = true);

}  // namespace h264
}  // namespace proto
}  // namespace usbdisplay
