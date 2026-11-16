// SPDX-License-Identifier: MIT
#include <vector>

#include "proto/nal_scan.h"
#include "test_harness.h"
#include "usbdisplay/protocol.h"

namespace {

void nal(std::vector<uint8_t>* out, uint8_t header,
         const std::vector<uint8_t>& payload, bool four_byte = false) {
  if (four_byte) out->push_back(0);
  out->push_back(0);
  out->push_back(0);
  out->push_back(1);
  out->push_back(header);
  out->insert(out->end(), payload.begin(), payload.end());
}

}  // namespace

int main() {
  using namespace usbdisplay::proto;
  {
    std::vector<uint8_t> stream;
    nal(&stream, 0x09, {0xF0});                 // AUD
    nal(&stream, 0x67, {0x11, 0x22, 0x33});     // SPS
    nal(&stream, 0x68, {0x44});                 // PPS
    nal(&stream, 0x65, {0x88, 0xAA, 0xBB}, true);  // IDR
    nal(&stream, 0x41, {0x88, 0xCC}, true);        // non-IDR
    nal(&stream, 0x67, {0x55});                 // SPS (next AU)
    nal(&stream, 0x68, {0x66});                 // PPS
    nal(&stream, 0x65, {0x88, 0xDD});           // IDR

    std::vector<annexb::AccessUnit> aus;
    std::string err;
    CHECK(annexb::split_access_units(stream.data(), stream.size(), &aus, &err));
    CHECK(aus.size() == 3);
    if (aus.size() == 3) {
      CHECK(aus[0].keyframe);
      CHECK(!aus[1].keyframe);
      CHECK(aus[2].keyframe);
      CHECK(aus[0].nals.size() == 4);  // AUD+SPS+PPS+IDR
      CHECK(aus[1].nals.size() == 1);
      CHECK(aus[2].nals.size() == 3);  // SPS+PPS+IDR
      CHECK(aus[0].data == stream.data());
      CHECK(aus[2].data + aus[2].size == stream.data() + stream.size());
      CHECK(annexb::describe_access_unit(aus[0]).find("5*") !=
            std::string::npos);
      // Root-cause guard: every access unit must keep its original Annex-B
      // start code (the previous black-screen bug dropped them).
      for (const auto& au : aus) {
        CHECK(au.size >= 4);
        CHECK(au.data[0] == 0 && au.data[1] == 0);
        CHECK((au.data[2] == 1) || (au.data[2] == 0 && au.data[3] == 1));
      }
      // The first NAL of every frame is the AUD (verified stream layout).
      std::vector<usbdisplay::proto::detail::NalRange> nals;
      CHECK(usbdisplay::proto::detail::scan_nal_units(
          aus[0].data, aus[0].size, &nals));
      CHECK(!nals.empty() && nals[0].type == 9);
      // Concatenating the AUs must reproduce the input byte-for-byte: no
      // start code or parameter-set byte may be lost or reordered.
      std::vector<uint8_t> rebuilt;
      for (const auto& au : aus) {
        rebuilt.insert(rebuilt.end(), au.data, au.data + au.size);
      }
      CHECK(rebuilt == stream);
    }
  }
  {
    // Input without a start code must be rejected, not silently accepted.
    const std::vector<uint8_t> junk = {0x11, 0x22, 0x33, 0x44, 0x55};
    std::vector<annexb::AccessUnit> aus;
    std::string err;
    CHECK(!annexb::split_access_units(junk.data(), junk.size(), &aus, &err));
    CHECK(!err.empty());
  }
  {
    // Parameter sets only (no VCL) must not produce a bogus access unit.
    std::vector<uint8_t> stream;
    nal(&stream, 0x67, {0x11});
    nal(&stream, 0x68, {0x22});
    std::vector<annexb::AccessUnit> aus;
    std::string err;
    CHECK(!annexb::split_access_units(stream.data(), stream.size(), &aus,
                                      &err));
    CHECK(aus.empty());
  }
  return testharness::finish("test_annexb");
}
