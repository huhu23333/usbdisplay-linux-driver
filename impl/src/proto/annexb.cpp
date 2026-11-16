// SPDX-License-Identifier: MIT
#include "usbdisplay/protocol.h"

#include "proto/nal_scan.h"
#include "common/util.h"

namespace usbdisplay {
namespace proto {
namespace annexb {

bool split_access_units(const uint8_t* data, size_t len,
                        std::vector<AccessUnit>* out, std::string* err) {
  out->clear();
  if (data == nullptr || len == 0) {
    if (err) *err = "annexb: empty input";
    return false;
  }
  std::vector<detail::NalRange> nals;
  if (!detail::scan_nal_units(data, len, &nals)) {
    if (err) *err = "annexb: no start code found (not Annex-B?)";
    return false;
  }

  struct Builder {
    size_t first_start = 0;
    size_t end = 0;
    bool has_vcl = false;
    bool keyframe = false;
    std::vector<NalUnit> nals;
  };
  Builder cur;
  bool have_cur = false;

  auto flush = [&]() {
    if (!have_cur || !cur.has_vcl) {
      // 丢弃末尾没有跟图像的参数集，避免产生空访问单元。
      have_cur = false;
      return;
    }
    AccessUnit au;
    au.data = data + cur.first_start;
    au.size = cur.end - cur.first_start;
    au.keyframe = cur.keyframe;
    au.nals = std::move(cur.nals);
    out->push_back(std::move(au));
    have_cur = false;
  };

  for (const auto& r : nals) {
    const bool vcl = detail::is_vcl_type(r.type);
    const bool aud = (r.type == 9);
    bool boundary = false;
    if (!have_cur) {
      boundary = true;
    } else if (aud) {
      boundary = true;
    } else if (vcl) {
      const uint8_t* nal_header = data + r.payload_offset;
      const size_t rbsp_off = r.payload_offset + 1;
      const bool first_mb_zero =
          (r.size > 1) &&
          detail::first_mb_in_slice_is_zero(data + rbsp_off, r.size - 1);
      if (cur.has_vcl && first_mb_zero) boundary = true;
      (void)nal_header;
    } else if (cur.has_vcl) {
      // 编码图像之后的参数集与 SEI 属于下一个访问单元。
      boundary = true;
    }

    if (boundary && have_cur) flush();
    if (!have_cur) {
      cur = Builder();
      cur.first_start = r.start_code_offset;
      have_cur = true;
    }
    cur.end = r.payload_offset + r.size;

    NalUnit nal;
    nal.data = data + r.start_code_offset;
    nal.size = (r.payload_offset + r.size) - r.start_code_offset;
    nal.type = r.type;
    nal.is_vcl = vcl;
    nal.is_keyframe = (r.type == 5);
    if (vcl && r.type == 5) {
      cur.keyframe = true;
    }
    if (vcl) cur.has_vcl = true;
    cur.nals.push_back(nal);
  }
  flush();
  // 末尾状态已在刷新步骤中处理。
  if (out->empty()) {
    if (err) *err = "annexb: no access unit with VCL data found";
    return false;
  }
  return true;
}

std::string describe_access_unit(const AccessUnit& au) {
  std::string out;
  for (const auto& n : au.nals) {
    if (!out.empty()) out += ' ';
    out += std::to_string(static_cast<int>(n.type));
    if (n.is_vcl) out += "*";
  }
  return out;
}

}  // namespace annexb
}  // namespace proto
}  // namespace usbdisplay
