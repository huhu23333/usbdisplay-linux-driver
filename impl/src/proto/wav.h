// SPDX-License-Identifier: MIT
// 发送工具使用的精简 RIFF/WAVE 读取器，支持未压缩 PCM 与 32 位浮点，
// 包括 WAVE_FORMAT_EXTENSIBLE 格式。
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace usbdisplay {
namespace proto {
namespace wav {

struct WavInfo {
  uint16_t format_tag = 0;  // 1=PCM, 3=IEEE float (extensible resolved)
  uint16_t channels = 0;
  uint32_t sample_rate = 0;
  uint16_t bits_per_sample = 0;
  uint16_t block_align = 0;
  const uint8_t* data = nullptr;  // points at the data chunk payload
  size_t data_size = 0;
  size_t data_offset = 0;  // offset of data payload in the source buffer
  bool is_float = false;
};

bool parse(const uint8_t* bytes, size_t len, WavInfo* out, std::string* err);

// 转换为交织排列的 16 位有符号小端采样，不做重采样，
// 调用方需按文件中的采样率与声道数配置设备。
bool to_s16le(const WavInfo& info, std::vector<int16_t>* out, std::string* err);

}  // namespace wav
}  // namespace proto
}  // namespace usbdisplay
