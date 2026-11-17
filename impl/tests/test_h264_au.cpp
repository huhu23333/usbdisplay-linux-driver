// SPDX-License-Identifier: MIT
#include <cstdint>
#include <string>
#include <vector>

#include "evdi/h264_au.h"
#include "test_harness.h"

namespace {

std::vector<uint8_t> bytes(std::initializer_list<uint8_t> values) {
  return std::vector<uint8_t>(values);
}

}  // namespace

int main() {
  using namespace usbdisplay::evdi;
  {
    const std::vector<uint8_t> aud = bytes({0, 0, 0, 1, 0x09, 0xf0});
    const std::vector<uint8_t> sps = bytes({0, 0, 0, 1, 0x67, 0x42});
    const std::vector<uint8_t> idr = bytes({0, 0, 0, 1, 0x65, 0x88});
    std::vector<H264NalView> nals = {{aud.data(), aud.size()},
                                     {sps.data(), sps.size()},
                                     {idr.data(), idr.size()}};
    H264AccessUnit au;
    std::string err;
    CHECK(build_access_unit(nals, true, &au, &err));
    CHECK(au.keyframe);
    CHECK(au.data.size() == aud.size() + sps.size() + idr.size());
    CHECK(au.data[4] == 0x09);
    CHECK(au.data[10] == 0x67);
    CHECK(au.data[16] == 0x65);
  }
  {
    // 无起始码的 NAL 自动补 4 字节起始码；3 字节起始码原样保留。
    const std::vector<uint8_t> no_start = bytes({0x67, 0x42});
    const std::vector<uint8_t> short_start = bytes({0, 0, 1, 0x68, 0xce});
    std::vector<H264NalView> nals = {{no_start.data(), no_start.size()},
                                     {short_start.data(), short_start.size()}};
    H264AccessUnit au;
    std::string err;
    CHECK(build_access_unit(nals, false, &au, &err));
    CHECK(!au.keyframe);
    CHECK(au.data.size() == 4 + no_start.size() + short_start.size());
    CHECK(au.data[0] == 0 && au.data[3] == 1);
    CHECK(au.data[4] == 0x67);
  }
  {
    H264AccessUnit au;
    std::string err;
    CHECK(!build_access_unit({}, false, &au, &err));
    CHECK(!err.empty());
    std::vector<H264NalView> bad = {{nullptr, 0}};
    CHECK(!build_access_unit(bad, false, &au, &err));
  }
  return testharness::finish("test_h264_au");
}
