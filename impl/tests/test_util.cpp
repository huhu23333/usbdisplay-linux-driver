// SPDX-License-Identifier: MIT
#include <unistd.h>

#include <string>
#include <vector>

#include "common/util.h"
#include "test_harness.h"

int main() {
  using namespace usbdisplay;
  uint8_t buf[8] = {0};
  write_le16(buf, 0x1234);
  CHECK(buf[0] == 0x34 && buf[1] == 0x12);
  CHECK(read_le16(buf) == 0x1234);
  write_le32(buf, 0xDEADBEEF);
  CHECK(buf[0] == 0xEF && buf[3] == 0xDE);
  CHECK(read_le32(buf) == 0xDEADBEEF);
  write_le64(buf, 0x0102030405060708ULL);
  CHECK(buf[0] == 0x08 && buf[7] == 0x01);
  CHECK(read_le64(buf) == 0x0102030405060708ULL);
  write_be16(buf, 0x1234);
  CHECK(buf[0] == 0x12 && buf[1] == 0x34);
  CHECK(read_be16(buf) == 0x1234);
  write_be32(buf, 0xA1B2C3D4);
  CHECK(buf[0] == 0xA1 && read_be32(buf) == 0xA1B2C3D4);

  const uint8_t data[] = {0x00, 0x01, 0x02, 0x41, 0x42};
  CHECK(to_hex(data, sizeof(data)) == "0001024142");
  const std::string dump = hexdump(data, sizeof(data), 32);
  CHECK(dump.find("00 01 02 41 42") != std::string::npos);
  CHECK(dump.find("|...AB|") != std::string::npos);

  uint64_t value = 0;
  CHECK(parse_u64("1234", &value) && value == 1234);
  CHECK(parse_u64("0x10", &value) && value == 16);
  CHECK(!parse_u64("12x", &value));
  int ivalue = 0;
  CHECK(parse_int("-42", &ivalue) && ivalue == -42);
  CHECK(trim("  a b  ") == "a b");

  CHECK(mono_us() <= mono_us());
  CHECK(mono_ms() <= mono_us() / 1000 + 1);

  const std::string path =
      "/tmp/usbdisplay_test_util_" + std::to_string(::getpid()) + ".bin";
  std::string err;
  const std::vector<uint8_t> payload = {1, 2, 3, 4, 5};
  CHECK(write_file(path, payload.data(), payload.size(), &err));
  std::vector<uint8_t> readback;
  CHECK(read_file(path, &readback, &err));
  CHECK(readback == payload);
  ::unlink(path.c_str());
  CHECK(!read_file(path, &readback, &err));
  return testharness::finish("test_util");
}
