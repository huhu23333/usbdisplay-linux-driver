// SPDX-License-Identifier: MIT
#include "proto/nal_scan.h"

namespace usbdisplay {
namespace proto {
namespace detail {

bool scan_nal_units(const uint8_t* data, size_t len,
                    std::vector<NalRange>* out) {
  out->clear();
  if (data == nullptr || len < 3) return false;

  size_t i = 0;
  // Find the first start code.
  while (i + 3 <= len) {
    if (data[i] == 0 && data[i + 1] == 0 && data[i + 2] == 1) break;
    ++i;
  }
  if (i + 3 > len) return false;

  size_t start = i;
  size_t sc_size = (start > 0 && data[start - 1] == 0) ? 4 : 3;
  if (sc_size == 4) --start;
  size_t payload = i + 3;

  while (payload < len) {
    size_t next = payload;
    while (next + 3 <= len) {
      if (data[next] == 0 && data[next + 1] == 0 && data[next + 2] == 1) {
        break;
      }
      ++next;
    }
    size_t end;
    size_t next_sc_size = 0;
    size_t next_off = 0;
    if (next + 3 > len) {
      end = len;
    } else {
      next_off = next;
      next_sc_size = (next > 0 && data[next - 1] == 0) ? 4 : 3;
      if (next_sc_size == 4) --next_off;
      end = next_off;
    }
    if (end > payload) {
      NalRange r;
      r.start_code_offset = start;
      r.start_code_size = sc_size;
      r.payload_offset = payload;
      r.size = end - payload;
      r.type = static_cast<uint8_t>(data[payload] & 0x1f);
      out->push_back(r);
    }
    if (next + 3 > len) break;
    start = next_off;
    sc_size = next_sc_size;
    payload = next + 3;
  }
  return !out->empty();
}

bool is_vcl_type(uint8_t type) { return type >= 1 && type <= 5; }

bool first_mb_in_slice_is_zero(const uint8_t* p, size_t len) {
  if (p == nullptr || len == 0) return false;
  // ue(v): count leading zero bits, then a 1 bit. value = 2^n - 1 + suffix.
  // first_mb_in_slice == 0 => the very first bit is 1.
  return (p[0] & 0x80) != 0;
}

}  // namespace detail
}  // namespace proto
}  // namespace usbdisplay
