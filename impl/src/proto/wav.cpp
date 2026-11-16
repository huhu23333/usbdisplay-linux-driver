// SPDX-License-Identifier: MIT
#include "proto/wav.h"

#include <algorithm>
#include <cmath>
#include <cstring>

#include "common/util.h"

namespace usbdisplay {
namespace proto {
namespace wav {
namespace {

uint32_t rd32(const uint8_t* p) { return read_le32(p); }
uint16_t rd16(const uint8_t* p) { return read_le16(p); }

}  // namespace

bool parse(const uint8_t* bytes, size_t len, WavInfo* out, std::string* err) {
  *out = WavInfo();
  if (len < 12 || std::memcmp(bytes, "RIFF", 4) != 0 ||
      std::memcmp(bytes + 8, "WAVE", 4) != 0) {
    if (err) *err = "not a RIFF/WAVE file";
    return false;
  }
  size_t pos = 12;
  bool have_fmt = false;
  bool have_data = false;
  while (pos + 8 <= len) {
    const char* id = reinterpret_cast<const char*>(bytes + pos);
    const uint32_t size = rd32(bytes + pos + 4);
    const size_t body = pos + 8;
    if (body + size > len) {
      // Tolerate truncated final chunk only if it is data.
      if (std::memcmp(id, "data", 4) != 0) {
        if (err) *err = "truncated WAV chunk " + std::string(id, 4);
        return false;
      }
    }
    if (std::memcmp(id, "fmt ", 4) == 0 && size >= 16) {
      out->format_tag = rd16(bytes + body);
      out->channels = rd16(bytes + body + 2);
      out->sample_rate = rd32(bytes + body + 4);
      out->block_align = rd16(bytes + body + 12);
      out->bits_per_sample = rd16(bytes + body + 14);
      if (out->format_tag == 0xFFFE && size >= 40) {
        // WAVE_FORMAT_EXTENSIBLE: subformat GUID starts at body+24; first two
        // bytes are the actual format tag.
        out->format_tag = rd16(bytes + body + 24);
      }
      have_fmt = true;
    } else if (std::memcmp(id, "data", 4) == 0) {
      out->data = bytes + body;
      out->data_offset = body;
      out->data_size = std::min<size_t>(size, len - body);
      have_data = true;
    }
    pos = body + size + (size & 1u);
  }
  if (!have_fmt || !have_data) {
    if (err) *err = "missing fmt or data chunk";
    return false;
  }
  if (out->format_tag != 1 && out->format_tag != 3) {
    if (err) {
      *err = "unsupported WAV format tag " + std::to_string(out->format_tag) +
             " (only PCM/float)";
    }
    return false;
  }
  out->is_float = (out->format_tag == 3);
  if (out->channels == 0 || out->sample_rate == 0 ||
      out->bits_per_sample == 0) {
    if (err) *err = "invalid WAV fmt fields";
    return false;
  }
  if (out->block_align == 0) {
    out->block_align = static_cast<uint16_t>(
        (out->bits_per_sample / 8u) * out->channels);
  }
  return true;
}

bool to_s16le(const WavInfo& info, std::vector<int16_t>* out,
              std::string* err) {
  const size_t bytes_per_sample = info.bits_per_sample / 8u;
  if (bytes_per_sample == 0 || info.block_align == 0) {
    if (err) *err = "invalid WAV sample layout";
    return false;
  }
  const size_t samples = info.data_size / bytes_per_sample;
  out->clear();
  out->reserve(samples);
  const uint8_t* p = info.data;
  for (size_t i = 0; i < samples; ++i, p += bytes_per_sample) {
    int32_t v = 0;
    if (info.is_float) {
      if (bytes_per_sample != 4) {
        if (err) *err = "only 32-bit float WAV is supported";
        return false;
      }
      float f;
      std::memcpy(&f, p, 4);
      f = std::max(-1.0f, std::min(1.0f, f));
      v = static_cast<int32_t>(std::lround(f * 32767.0f));
    } else {
      switch (bytes_per_sample) {
        case 1: v = (static_cast<int32_t>(p[0]) - 128) << 8; break;
        case 2: v = static_cast<int16_t>(read_le16(p)); break;
        case 3: {
          int32_t s = static_cast<int32_t>(p[0] | (p[1] << 8) | (p[2] << 16));
          if (s & 0x800000) s |= ~0xFFFFFF;  // sign extend
          v = s >> 8;
          break;
        }
        case 4: {
          const int32_t s = static_cast<int32_t>(read_le32(p));
          v = s >> 16;
          break;
        }
        default:
          if (err) *err = "unsupported WAV bit depth";
          return false;
      }
    }
    out->push_back(static_cast<int16_t>(std::max(-32768, std::min(32767, v))));
  }
  return true;
}

}  // namespace wav
}  // namespace proto
}  // namespace usbdisplay
