// SPDX-License-Identifier: MIT
// Annex-B NAL 单元扫描，供访问单元拆分与 RTP 打包共用。
#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace usbdisplay {
namespace proto {
namespace detail {

struct NalRange {
  size_t start_code_offset = 0;  // offset of 00 00 01 / 00 00 00 01
  size_t start_code_size = 0;    // 3 or 4
  size_t payload_offset = 0;     // offset of the NAL header byte
  size_t size = 0;               // NAL unit size including header byte
  uint8_t type = 0;              // nal_unit_type
};

// 扫描 Annex-B 字节流，找不到起始码时返回假。
bool scan_nal_units(const uint8_t* data, size_t len,
                    std::vector<NalRange>* out);

// 判断 NAL 负载中的 first_mb_in_slice 是否为零。
bool first_mb_in_slice_is_zero(const uint8_t* nal_payload, size_t len);

// 判断 NAL 类型是否属于编码切片。
bool is_vcl_type(uint8_t type);

}  // namespace detail
}  // namespace proto
}  // namespace usbdisplay
