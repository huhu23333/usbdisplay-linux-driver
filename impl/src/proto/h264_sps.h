// SPDX-License-Identifier: MIT
// Minimal H.264 SPS parser used to derive the *real* frame rate from the VUI
// timing info (num_units_in_tick / time_scale). Verified capture SPS: Main
// profile, timing_info_present=1, num_units_in_tick=1, time_scale=120
// -> 120 / (2*1) = 60 fps.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace usbdisplay {
namespace proto {
namespace h264 {

struct SpsInfo {
  uint8_t profile_idc = 0;
  uint8_t level_idc = 0;
  uint32_t width = 0;
  uint32_t height = 0;
  bool vui_present = false;
  bool timing_present = false;
  uint32_t num_units_in_tick = 0;
  uint32_t time_scale = 0;
  bool fixed_frame_rate = false;
  // Derived when timing_present: time_scale / (2 * num_units_in_tick).
  double fps = 0.0;
};

// nal points at the NAL header byte (type 7) and len includes it. Handles
// profile-specific fields and emulation-prevention bytes.
bool parse_sps(const uint8_t* nal, size_t len, SpsInfo* out, std::string* err);

// Scan an Annex-B access unit / stream for the first SPS and return its VUI
// frame rate. Returns false when no SPS/timing info is found.
bool extract_fps_from_annexb(const uint8_t* data, size_t len, double* fps,
                             SpsInfo* info, std::string* err);

// Build the SPS RBSP data bits (without start code) for tests/diagnostics.
// This is a convenience for synthetic streams; it is not needed for sending.
std::vector<uint8_t> make_test_sps(uint32_t width, uint32_t height,
                                   uint32_t num_units_in_tick,
                                   uint32_t time_scale, bool with_vui = true);

}  // namespace h264
}  // namespace proto
}  // namespace usbdisplay
