// SPDX-License-Identifier: MIT
#include <cmath>
#include <vector>

#include "common/util.h"
#include "proto/h264_sps.h"
#include "test_harness.h"

int main() {
  using namespace usbdisplay::proto::h264;
  {
    // Verified capture characteristics: Main, level 4.2, 1920x1080, VUI
    // timing 1/120 -> 60 fps.
    const std::vector<uint8_t> sps = make_test_sps(1920, 1080, 1, 120, true);
    SpsInfo info;
    std::string err;
    CHECK(parse_sps(sps.data(), sps.size(), &info, &err));
    CHECK(info.profile_idc == 77);
    CHECK(info.level_idc == 42);
    CHECK(info.width == 1920);
    CHECK(info.height == 1080);
    CHECK(info.vui_present);
    CHECK(info.timing_present);
    CHECK(info.num_units_in_tick == 1);
    CHECK(info.time_scale == 120);
    CHECK(info.fixed_frame_rate);
    CHECK(std::fabs(info.fps - 60.0) < 1e-6);
  }
  {
    // 30 fps VUI.
    const std::vector<uint8_t> sps = make_test_sps(1280, 720, 1, 60, true);
    SpsInfo info;
    std::string err;
    CHECK(parse_sps(sps.data(), sps.size(), &info, &err));
    CHECK(std::fabs(info.fps - 30.0) < 1e-6);
    CHECK(info.width == 1280 && info.height == 720);
  }
  {
    // 29.97 fps VUI (60000/1001).
    const std::vector<uint8_t> sps =
        make_test_sps(1920, 1080, 1001, 60000, true);
    SpsInfo info;
    std::string err;
    CHECK(parse_sps(sps.data(), sps.size(), &info, &err));
    CHECK(std::fabs(info.fps - 29.97002997) < 0.01);
  }
  {
    // Annex-B extraction (start code + SPS) and a missing-VUI case.
    const std::vector<uint8_t> sps = make_test_sps(1920, 1080, 1, 120, true);
    std::vector<uint8_t> stream = {0, 0, 0, 1};
    stream.insert(stream.end(), sps.begin(), sps.end());
    stream.insert(stream.end(), {0, 0, 1, 0x65, 0x88, 0xAA});
    double fps = 0.0;
    SpsInfo info;
    std::string err;
    CHECK(extract_fps_from_annexb(stream.data(), stream.size(), &fps, &info,
                                  &err));
    CHECK(std::fabs(fps - 60.0) < 1e-6);
    CHECK(info.width == 1920);

    const std::vector<uint8_t> no_vui =
        make_test_sps(1920, 1080, 1, 120, false);
    std::vector<uint8_t> stream2 = {0, 0, 0, 1};
    stream2.insert(stream2.end(), no_vui.begin(), no_vui.end());
    fps = 0.0;
    CHECK(!extract_fps_from_annexb(stream2.data(), stream2.size(), &fps, nullptr,
                                   &err));
    CHECK(std::fabs(fps) < 1e-9);
    CHECK(!err.empty());
  }
  {
    // Malformed inputs are rejected rather than crashing.
    SpsInfo info;
    std::string err;
    const uint8_t junk[] = {0x67, 0x4D, 0x40};
    CHECK(!parse_sps(junk, sizeof(junk), &info, &err));
    const uint8_t not_sps[] = {0x41, 0x88, 0xAA, 0xBB};
    CHECK(!parse_sps(not_sps, sizeof(not_sps), &info, &err));
    CHECK(!parse_sps(nullptr, 0, &info, &err));
  }
  return testharness::finish("test_h264_sps");
}
