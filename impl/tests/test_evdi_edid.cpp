// SPDX-License-Identifier: MIT
#include <cstdint>
#include <string>
#include <tuple>
#include <vector>

#include "evdi/edid.h"
#include "test_harness.h"

namespace {
std::tuple<uint32_t, uint32_t, uint32_t, uint32_t> timing(
    const std::vector<uint8_t>& value, size_t offset) {
  const uint32_t clock = static_cast<uint32_t>(value[offset]) |
                         (static_cast<uint32_t>(value[offset + 1]) << 8);
  const uint32_t hblank = static_cast<uint32_t>(value[offset + 3]) |
                          (static_cast<uint32_t>(value[offset + 4] & 0x0f) << 8);
  const uint32_t vblank = static_cast<uint32_t>(value[offset + 6]) |
                          (static_cast<uint32_t>(value[offset + 7] & 0x0f) << 8);
  const uint32_t htotal = 1920 + hblank;
  const uint32_t vtotal = 1080 + vblank;
  return std::make_tuple(clock, htotal, vtotal,
                         (clock * 10000U) / (htotal * vtotal));
}
}  // namespace

int main() {
  using namespace usbdisplay::evdi;
  const std::vector<uint8_t> edid = make_edid_1920x1080();
  CHECK(edid.size() == 128);
  CHECK(edid[0] == 0x00 && edid[1] == 0xff && edid[7] == 0x00);
  uint32_t sum = 0;
  for (uint8_t byte : edid) sum += byte;
  CHECK((sum & 0xff) == 0);

  const auto mode60 = timing(edid, 54);
  CHECK(std::get<0>(mode60) == 14850);
  CHECK(std::get<1>(mode60) == 2200);
  CHECK(std::get<2>(mode60) == 1125);
  CHECK(std::get<3>(mode60) == 60);

  const auto mode120 = timing(edid, 90);
  CHECK(std::get<0>(mode120) == 29700);
  CHECK(std::get<1>(mode120) == 2200);
  CHECK(std::get<2>(mode120) == 1125);
  CHECK(std::get<3>(mode120) == 120);

  // monitor name 在 72，range 在 108，extension 计数 0，checksum 已校验。
  CHECK(edid[72] == 0x00 && edid[73] == 0x00 && edid[74] == 0x00 &&
        edid[75] == 0xfc);
  const std::string name(reinterpret_cast<const char*>(&edid[77]), 13);
  CHECK(name.find("USBDisplay") != std::string::npos);
  CHECK(edid[108] == 0x00 && edid[111] == 0xfd);
  CHECK(edid[126] == 0);

  CHECK(make_edid_1920x1080() == edid);
  CHECK(make_edid_1920x1080_60() == edid);
  CHECK(make_edid_1920x1080_120() == edid);
  return testharness::finish("test_evdi_edid");
}
