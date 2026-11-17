// SPDX-License-Identifier: MIT
#include "evdi/edid.h"

#include <cstring>

namespace usbdisplay {
namespace evdi {
namespace {

void put_le16(std::vector<uint8_t>* out, size_t offset, uint16_t value) {
  (*out)[offset] = static_cast<uint8_t>(value & 0xff);
  (*out)[offset + 1] = static_cast<uint8_t>((value >> 8) & 0xff);
}

void put_descriptor_header(std::vector<uint8_t>* out, size_t offset,
                           uint8_t tag) {
  (*out)[offset] = 0;
  (*out)[offset + 1] = 0;
  (*out)[offset + 2] = 0;
  (*out)[offset + 3] = tag;
  (*out)[offset + 4] = 0;
}

void put_text_descriptor(std::vector<uint8_t>* out, size_t offset,
                         uint8_t tag, const char* text) {
  put_descriptor_header(out, offset, tag);
  size_t i = 0;
  for (; i < 13 && text[i] != '\0'; ++i) {
    (*out)[offset + 5 + i] = static_cast<uint8_t>(text[i]);
  }
  if (i < 13) (*out)[offset + 5 + i] = '\n';
  for (size_t j = i + 1; j < 13; ++j) (*out)[offset + 5 + j] = ' ';
}

void put_1080p_dtd(std::vector<uint8_t>* edid, size_t offset, uint16_t clock_10khz) {
  put_le16(edid, offset, clock_10khz);
  (*edid)[offset + 2] = 1920 & 0xff;
  (*edid)[offset + 3] = 280 & 0xff;
  (*edid)[offset + 4] = static_cast<uint8_t>(((1920 >> 8) << 4) | (280 >> 8));
  (*edid)[offset + 5] = 1080 & 0xff;
  (*edid)[offset + 6] = 45 & 0xff;
  (*edid)[offset + 7] = static_cast<uint8_t>(((1080 >> 8) << 4) | (45 >> 8));
  (*edid)[offset + 8] = 88;
  (*edid)[offset + 9] = 44;
  (*edid)[offset + 10] = static_cast<uint8_t>((4 << 4) | 5);
  (*edid)[offset + 11] = 0;
  (*edid)[offset + 12] = 509 & 0xff;
  (*edid)[offset + 13] = 286 & 0xff;
  (*edid)[offset + 14] = static_cast<uint8_t>(((509 >> 8) << 4) | (286 >> 8));
  (*edid)[offset + 15] = 0;
  (*edid)[offset + 16] = 0;
  (*edid)[offset + 17] = 0x1a;
}

}  // namespace

std::vector<uint8_t> make_edid_1920x1080(int) {
  std::vector<uint8_t> edid(128, 0);
  const uint8_t header[8] = {0x00, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x00};
  std::memcpy(edid.data(), header, sizeof(header));
  put_le16(&edid, 8, 0x5493);
  put_le16(&edid, 10, 0x0201);
  put_le16(&edid, 12, 0x0001);
  edid[16] = 1;
  edid[17] = 34;
  edid[18] = 1;
  edid[19] = 3;
  edid[20] = 0x80;
  edid[21] = 51;
  edid[22] = 29;
  edid[23] = 120;
  edid[24] = 0x0a;
  const uint8_t chromaticity[10] = {0xee, 0x91, 0xa3, 0x54, 0x4c,
                                    0x99, 0x26, 0x0f, 0x50, 0x54};
  std::memcpy(edid.data() + 25, chromaticity, sizeof(chromaticity));
  for (size_t i = 38; i < 54; ++i) edid[i] = 0x01;

  // 两个 DTD：54=1080p60 148.5MHz，90=1080p120 297MHz，总时序均 2200x1125。
  put_1080p_dtd(&edid, 54, 14850);
  put_1080p_dtd(&edid, 90, 29700);
  put_text_descriptor(&edid, 72, 0xfc, "USBDisplay");

  const size_t range = 108;
  put_descriptor_header(&edid, range, 0xfd);
  edid[range + 5] = 48;
  edid[range + 6] = 144;
  edid[range + 7] = 30;
  edid[range + 8] = 90;
  edid[range + 9] = 30;

  edid[126] = 0;
  uint8_t sum = 0;
  for (size_t i = 0; i < 127; ++i) sum = static_cast<uint8_t>(sum + edid[i]);
  edid[127] = static_cast<uint8_t>(0x100 - sum);
  return edid;
}

std::vector<uint8_t> make_edid_1920x1080() { return make_edid_1920x1080(60); }
std::vector<uint8_t> make_edid_1920x1080_60() { return make_edid_1920x1080(60); }
std::vector<uint8_t> make_edid_1920x1080_120() { return make_edid_1920x1080(120); }

}  // namespace evdi
}  // namespace usbdisplay
