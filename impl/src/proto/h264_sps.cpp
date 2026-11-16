// SPDX-License-Identifier: MIT
#include "proto/h264_sps.h"

#include <cstring>
#include <vector>

#include "common/util.h"
#include "proto/nal_scan.h"

namespace usbdisplay {
namespace proto {
namespace h264 {
namespace {

class BitReader {
 public:
  explicit BitReader(const std::vector<uint8_t>& data) : data_(data) {}

  bool u(int bits, uint32_t* out) {
    if (bits < 0 || bits > 32) return false;
    uint32_t value = 0;
    for (int i = 0; i < bits; ++i) {
      if (bit_pos_ >= data_.size() * 8) return false;
      const size_t byte = bit_pos_ / 8;
      const int shift = 7 - static_cast<int>(bit_pos_ % 8);
      value = (value << 1) | ((data_[byte] >> shift) & 1u);
      ++bit_pos_;
    }
    *out = value;
    return true;
  }

  bool ue(uint32_t* out) {
    int leading_zero_bits = 0;
    uint32_t bit = 0;
    for (;;) {
      if (!u(1, &bit)) return false;
      if (bit != 0) break;
      ++leading_zero_bits;
      if (leading_zero_bits > 31) return false;
    }
    uint32_t suffix = 0;
    if (leading_zero_bits > 0) {
      if (!u(leading_zero_bits, &suffix)) return false;
    }
    *out = ((1u << leading_zero_bits) - 1u) + suffix;
    return true;
  }

  bool se(int32_t* out) {
    uint32_t value = 0;
    if (!ue(&value)) return false;
    if (value % 2 == 1) {
      *out = static_cast<int32_t>((value + 1) / 2);
    } else {
      *out = -static_cast<int32_t>(value / 2);
    }
    return true;
  }

  bool skip_bits(int bits) {
    uint32_t ignored = 0;
    return u(bits, &ignored);
  }

 private:
  const std::vector<uint8_t>& data_;
  size_t bit_pos_ = 0;
};

// 在零字节序列后插入防竞争字节，把 RBSP 转换为 EBSP。
std::vector<uint8_t> to_ebsp(const std::vector<uint8_t>& rbsp) {
  std::vector<uint8_t> out;
  out.reserve(rbsp.size() + 8);
  int zeros = 0;
  for (uint8_t byte : rbsp) {
    if (zeros >= 2 && byte <= 0x03) {
      out.push_back(0x03);
      zeros = 0;
    }
    out.push_back(byte);
    if (byte == 0) {
      ++zeros;
    } else {
      zeros = 0;
    }
  }
  return out;
}

bool skip_scaling_list(BitReader* reader, int size) {
  int last_scale = 8;
  int next_scale = 8;
  for (int j = 0; j < size; ++j) {
    if (next_scale != 0) {
      int32_t delta = 0;
      if (!reader->se(&delta)) return false;
      next_scale = (last_scale + delta + 256) % 256;
    }
    last_scale = (next_scale == 0) ? last_scale : next_scale;
  }
  return true;
}

bool is_high_profile(uint32_t profile_idc) {
  switch (profile_idc) {
    case 100: case 110: case 122: case 244: case 44:
    case 83: case 86: case 118: case 128: case 138:
    case 139: case 134: case 135:
      return true;
    default:
      return false;
  }
}

}  // namespace

bool parse_sps(const uint8_t* nal, size_t len, SpsInfo* out, std::string* err) {
  *out = SpsInfo();
  if (nal == nullptr || len < 4) {
    if (err) *err = "SPS too short";
    return false;
  }
  if ((nal[0] & 0x1f) != 7) {
    if (err) *err = "not an SPS NAL unit";
    return false;
  }
  // 逐位解析前先去除防竞争字节。
  std::vector<uint8_t> rbsp;
  rbsp.reserve(len);
  int zeros = 0;
  for (size_t i = 1; i < len; ++i) {  // skip NAL header
    const uint8_t byte = nal[i];
    if (zeros >= 2 && byte == 0x03) {
      zeros = 0;
      continue;
    }
    rbsp.push_back(byte);
    if (byte == 0) {
      ++zeros;
    } else {
      zeros = 0;
    }
  }
  BitReader reader(rbsp);
  uint32_t value = 0;
  if (!reader.u(8, &value)) return false;
  out->profile_idc = static_cast<uint8_t>(value);
  if (!reader.u(8, &value)) return false;  // constraint flags + reserved
  if (!reader.u(8, &value)) return false;
  out->level_idc = static_cast<uint8_t>(value);
  if (!reader.ue(&value)) return false;  // seq_parameter_set_id

  uint32_t chroma_format_idc = 1;
  if (is_high_profile(out->profile_idc)) {
    if (!reader.ue(&chroma_format_idc)) return false;
    if (chroma_format_idc == 3) {
      if (!reader.skip_bits(1)) return false;
    }
    if (!reader.ue(&value)) return false;  // bit_depth_luma_minus8
    if (!reader.ue(&value)) return false;  // bit_depth_chroma_minus8
    if (!reader.skip_bits(1)) return false;  // qpprime_y_zero_transform_bypass
    uint32_t scaling_matrix_present = 0;
    if (!reader.u(1, &scaling_matrix_present)) return false;
    if (scaling_matrix_present) {
      const int lists = (chroma_format_idc != 3) ? 8 : 12;
      for (int i = 0; i < lists; ++i) {
        uint32_t present = 0;
        if (!reader.u(1, &present)) return false;
        if (present) {
          if (!skip_scaling_list(&reader, i < 6 ? 16 : 64)) return false;
        }
      }
    }
  }
  if (!reader.ue(&value)) return false;  // log2_max_frame_num_minus4
  uint32_t poc_type = 0;
  if (!reader.ue(&poc_type)) return false;
  if (poc_type == 0) {
    if (!reader.ue(&value)) return false;
  } else if (poc_type == 1) {
    if (!reader.skip_bits(1)) return false;
    int32_t se = 0;
    if (!reader.se(&se)) return false;
    if (!reader.se(&se)) return false;
    uint32_t num_ref = 0;
    if (!reader.ue(&num_ref)) return false;
    for (uint32_t i = 0; i < num_ref; ++i) {
      if (!reader.se(&se)) return false;
    }
  }
  if (!reader.ue(&value)) return false;  // max_num_ref_frames
  if (!reader.skip_bits(1)) return false;  // gaps_in_frame_num_value_allowed
  uint32_t width_mbs = 0;
  uint32_t height_map_units = 0;
  if (!reader.ue(&width_mbs)) return false;
  if (!reader.ue(&height_map_units)) return false;
  uint32_t frame_mbs_only = 0;
  if (!reader.u(1, &frame_mbs_only)) return false;
  if (frame_mbs_only == 0) {
    if (!reader.skip_bits(1)) return false;
  }
  if (!reader.skip_bits(1)) return false;  // direct_8x8_inference_flag
  uint32_t cropping = 0;
  uint32_t crop_left = 0, crop_right = 0, crop_top = 0, crop_bottom = 0;
  if (!reader.u(1, &cropping)) return false;
  if (cropping) {
    if (!reader.ue(&crop_left)) return false;
    if (!reader.ue(&crop_right)) return false;
    if (!reader.ue(&crop_top)) return false;
    if (!reader.ue(&crop_bottom)) return false;
  }
  uint32_t chroma_array_type = chroma_format_idc;
  uint32_t crop_unit_x = (chroma_array_type == 0) ? 1 : 2;
  uint32_t crop_unit_y =
      (chroma_array_type == 0 ? 1 : 2) * (2 - frame_mbs_only);
  out->width = (width_mbs + 1) * 16 - crop_unit_x * (crop_left + crop_right);
  out->height = (height_map_units + 1) * 16 * (2 - frame_mbs_only) -
                crop_unit_y * (crop_top + crop_bottom);

  uint32_t vui_present = 0;
  if (!reader.u(1, &vui_present)) return false;
  out->vui_present = vui_present != 0;
  if (!vui_present) return true;

  uint32_t flag = 0;
  if (!reader.u(1, &flag)) return false;  // aspect_ratio_info_present
  if (flag) {
    uint32_t aspect_ratio_idc = 0;
    if (!reader.u(8, &aspect_ratio_idc)) return false;
    if (aspect_ratio_idc == 255) {
      if (!reader.skip_bits(16) || !reader.skip_bits(16)) return false;
    }
  }
  if (!reader.u(1, &flag)) return false;  // overscan_info_present
  if (flag) {
    if (!reader.skip_bits(1)) return false;
  }
  if (!reader.u(1, &flag)) return false;  // video_signal_type_present
  if (flag) {
    if (!reader.skip_bits(3)) return false;  // video_format
    if (!reader.skip_bits(1)) return false;  // video_full_range
    uint32_t colour = 0;
    if (!reader.u(1, &colour)) return false;
    if (colour) {
      if (!reader.skip_bits(8) || !reader.skip_bits(8) ||
          !reader.skip_bits(8)) {
        return false;
      }
    }
  }
  if (!reader.u(1, &flag)) return false;  // chroma_loc_info_present
  if (flag) {
    if (!reader.ue(&value) || !reader.ue(&value)) return false;
  }
  uint32_t timing_present = 0;
  if (!reader.u(1, &timing_present)) return false;
  out->timing_present = timing_present != 0;
  if (timing_present) {
    if (!reader.u(32, &out->num_units_in_tick)) return false;
    if (!reader.u(32, &out->time_scale)) return false;
    uint32_t fixed = 0;
    if (!reader.u(1, &fixed)) return false;
    out->fixed_frame_rate = fixed != 0;
    if (out->num_units_in_tick != 0) {
      out->fps = static_cast<double>(out->time_scale) /
                 (2.0 * static_cast<double>(out->num_units_in_tick));
    }
  }
  return true;
}

bool extract_fps_from_annexb(const uint8_t* data, size_t len, double* fps,
                             SpsInfo* info, std::string* err) {
  std::vector<detail::NalRange> nals;
  if (!detail::scan_nal_units(data, len, &nals)) {
    if (err) *err = "no Annex-B NAL units found";
    return false;
  }
  for (const auto& nal : nals) {
    if (nal.type != 7 || nal.size == 0) continue;
    SpsInfo parsed;
    std::string perr;
    if (!parse_sps(data + nal.payload_offset, nal.size, &parsed, &perr)) {
      if (err) *err = "SPS parse: " + perr;
      continue;
    }
    if (!parsed.timing_present || parsed.fps <= 0.0) {
      if (info) *info = parsed;
      if (err) *err = "SPS has no VUI timing info";
      return false;
    }
    if (fps) *fps = parsed.fps;
    if (info) *info = parsed;
    return true;
  }
  if (err) *err = "no SPS NAL unit found";
  return false;
}

std::vector<uint8_t> make_test_sps(uint32_t width, uint32_t height,
                                   uint32_t num_units_in_tick,
                                   uint32_t time_scale, bool with_vui) {
  std::vector<uint8_t> bits;
  // 先累积比特，最后统一打包为字节。
  bits.clear();
  std::vector<uint8_t> bit_values;
  auto put = [&bit_values](uint32_t value, int count) {
    for (int i = count - 1; i >= 0; --i) {
      bit_values.push_back(static_cast<uint8_t>((value >> i) & 1u));
    }
  };
  auto put_ue = [&put](uint32_t value) {
    const uint32_t v = value + 1;
    int bits = 0;
    while ((1u << (bits + 1)) <= v && bits < 31) ++bits;
    for (int i = 0; i < bits; ++i) put(0, 1);
    for (int i = bits; i >= 0; --i) put((v >> i) & 1u, 1);
  };
  put(77, 8);   // profile_idc Main
  put(0x40, 8); // constraint flags
  put(42, 8);   // level_idc 4.2
  put_ue(0);    // sps_id
  put_ue(4);    // log2_max_frame_num_minus4
  put_ue(2);    // pic_order_cnt_type = 2
  put_ue(1);    // max_num_ref_frames
  put(0, 1);    // gaps_in_frame_num_value_allowed
  put_ue(width / 16 - 1);
  put_ue((height + 15) / 16 - 1);
  put(1, 1);  // frame_mbs_only_flag
  put(1, 1);  // direct_8x8_inference_flag
  const uint32_t crop_bottom = ((height + 15) / 16) * 16 - height;
  put(crop_bottom > 0 ? 1 : 0, 1);
  if (crop_bottom > 0) {
    put_ue(0);  // left
    put_ue(0);  // right
    put_ue(0);  // top
    put_ue(crop_bottom / 2);
  }
  put(with_vui ? 1 : 0, 1);
  if (with_vui) {
    put(0, 1);  // aspect_ratio_info_present
    put(0, 1);  // overscan_info_present
    put(0, 1);  // video_signal_type_present
    put(0, 1);  // chroma_loc_info_present
    put(1, 1);  // timing_info_present
    put(num_units_in_tick, 32);
    put(time_scale, 32);
    put(1, 1);  // fixed_frame_rate_flag
    put(0, 1);  // nal_hrd_parameters_present
    put(0, 1);  // vcl_hrd_parameters_present
    put(0, 1);  // pic_struct_present
    put(0, 1);  // bitstream_restriction_flag
  }
  // 写入结束位与填充零位。
  put(1, 1);
  while (bit_values.size() % 8 != 0) bit_values.push_back(0);
  std::vector<uint8_t> rbsp;
  rbsp.reserve(bit_values.size() / 8);
  for (size_t i = 0; i < bit_values.size(); i += 8) {
    uint8_t byte = 0;
    for (int j = 0; j < 8; ++j) byte = (byte << 1) | bit_values[i + j];
    rbsp.push_back(byte);
  }
  std::vector<uint8_t> ebsp = to_ebsp(rbsp);
  std::vector<uint8_t> nal;
  nal.reserve(ebsp.size() + 1);
  nal.push_back(0x67);  // NAL header: type 7, nal_ref_idc 3
  nal.insert(nal.end(), ebsp.begin(), ebsp.end());
  return nal;
}

}  // namespace h264
}  // namespace proto
}  // namespace usbdisplay
