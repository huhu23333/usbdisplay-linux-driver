// SPDX-License-Identifier: MIT
#include <cstring>
#include <vector>

#include "common/util.h"
#include "test_harness.h"
#include "usbdisplay/protocol.h"
#include "proto/wav.h"

namespace {

std::vector<uint8_t> make_wav(uint16_t format_tag, uint16_t channels,
                              uint32_t rate, uint16_t bits,
                              const std::vector<uint8_t>& data) {
  const uint32_t data_size = static_cast<uint32_t>(data.size());
  std::vector<uint8_t> wav(44 + data_size, 0);
  std::memcpy(wav.data(), "RIFF", 4);
  usbdisplay::write_le32(wav.data() + 4, 36 + data_size);
  std::memcpy(wav.data() + 8, "WAVE", 4);
  std::memcpy(wav.data() + 12, "fmt ", 4);
  usbdisplay::write_le32(wav.data() + 16, 16);
  usbdisplay::write_le16(wav.data() + 20, format_tag);
  usbdisplay::write_le16(wav.data() + 22, channels);
  usbdisplay::write_le32(wav.data() + 24, rate);
  usbdisplay::write_le32(wav.data() + 28, rate * channels * bits / 8);
  usbdisplay::write_le16(wav.data() + 32,
                         static_cast<uint16_t>(channels * bits / 8));
  usbdisplay::write_le16(wav.data() + 34, bits);
  std::memcpy(wav.data() + 36, "data", 4);
  usbdisplay::write_le32(wav.data() + 40, data_size);
  std::memcpy(wav.data() + 44, data.data(), data.size());
  return wav;
}

}  // namespace

int main() {
  using namespace usbdisplay::proto;
  {
    const std::vector<uint8_t> data = {0x00, 0x00, 0x00, 0x40, 0x00, 0x80,
                                       0xFF, 0x7F};
    const auto wav = make_wav(1, 2, 48000, 16, data);
    wav::WavInfo info;
    std::string err;
    CHECK(wav::parse(wav.data(), wav.size(), &info, &err));
    CHECK(info.format_tag == 1 && info.channels == 2);
    CHECK(info.sample_rate == 48000 && info.bits_per_sample == 16);
    CHECK(info.data_size == data.size());
    std::vector<int16_t> samples;
    CHECK(wav::to_s16le(info, &samples, &err));
    CHECK(samples.size() == 4);
    CHECK(samples[0] == 0);
    CHECK(samples[1] == 16384);
    CHECK(samples[2] == -32768);
    CHECK(samples[3] == 32767);
  }
  {
    // 8 位无符号 PCM 转换为 16 位有符号采样。
    const std::vector<uint8_t> data = {0, 128, 255};
    const auto wav = make_wav(1, 1, 8000, 8, data);
    wav::WavInfo info;
    std::string err;
    CHECK(wav::parse(wav.data(), wav.size(), &info, &err));
    std::vector<int16_t> samples;
    CHECK(wav::to_s16le(info, &samples, &err));
    CHECK(samples.size() == 3);
    CHECK(samples[0] == -32768);
    CHECK(samples[1] == 0);
    CHECK(samples[2] == 32512);
  }
  {
    // 32 位浮点 PCM 的换算。
    std::vector<uint8_t> data(4);
    const float half = 0.5f;
    std::memcpy(data.data(), &half, 4);
    const auto wav = make_wav(3, 1, 44100, 32, data);
    wav::WavInfo info;
    std::string err;
    CHECK(wav::parse(wav.data(), wav.size(), &info, &err));
    CHECK(info.is_float);
    std::vector<int16_t> samples;
    CHECK(wav::to_s16le(info, &samples, &err));
    CHECK(samples.size() == 1);
    CHECK(samples[0] == 16384);
  }
  {
    // 24 位 PCM 的换算。
    const std::vector<uint8_t> data = {0x00, 0x00, 0x40};  // 0x400000
    const auto wav = make_wav(1, 1, 48000, 24, data);
    wav::WavInfo info;
    std::string err;
    CHECK(wav::parse(wav.data(), wav.size(), &info, &err));
    std::vector<int16_t> samples;
    CHECK(wav::to_s16le(info, &samples, &err));
    CHECK(samples.size() == 1 && samples[0] == 0x4000);
  }
  {
    const std::vector<uint8_t> junk = {'N', 'O', 'P', 'E'};
    wav::WavInfo info;
    std::string err;
    CHECK(!wav::parse(junk.data(), junk.size(), &info, &err));
    CHECK(!err.empty());
  }
  return testharness::finish("test_wav");
}
