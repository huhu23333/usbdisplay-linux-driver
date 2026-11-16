// SPDX-License-Identifier: MIT
// Minimal RIFF/WAVE reader for the sender tool. Supports uncompressed PCM
// (8/16/24/32-bit integer) and 32-bit float, including WAVE_FORMAT_EXTENSIBLE.
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

// Convert to interleaved signed 16-bit little-endian. No resampling is done;
// the caller must configure the device with the file's sample rate/channels.
bool to_s16le(const WavInfo& info, std::vector<int16_t>* out, std::string* err);

}  // namespace wav
}  // namespace proto
}  // namespace usbdisplay
