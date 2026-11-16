// SPDX-License-Identifier: MIT
// Internal: Annex-B NAL unit scanning shared by the Annex-B splitter and the
// RTP packetizer.
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

// Scan an Annex-B byte stream. Returns false when no start code was found.
bool scan_nal_units(const uint8_t* data, size_t len,
                    std::vector<NalRange>* out);

// H.264 first_mb_in_slice == 0 test on the RBSP bytes after the NAL header.
bool first_mb_in_slice_is_zero(const uint8_t* nal_payload, size_t len);

// NAL types considered VCL (coded slice).
bool is_vcl_type(uint8_t type);

}  // namespace detail
}  // namespace proto
}  // namespace usbdisplay
