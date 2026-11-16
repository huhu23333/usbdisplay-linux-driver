// SPDX-License-Identifier: MIT
#include "common/util.h"

#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>

namespace usbdisplay {

void write_le16(uint8_t* p, uint16_t v) {
  p[0] = static_cast<uint8_t>(v);
  p[1] = static_cast<uint8_t>(v >> 8);
}
void write_le32(uint8_t* p, uint32_t v) {
  p[0] = static_cast<uint8_t>(v);
  p[1] = static_cast<uint8_t>(v >> 8);
  p[2] = static_cast<uint8_t>(v >> 16);
  p[3] = static_cast<uint8_t>(v >> 24);
}
void write_le64(uint8_t* p, uint64_t v) {
  for (int i = 0; i < 8; ++i) p[i] = static_cast<uint8_t>(v >> (8 * i));
}
uint16_t read_le16(const uint8_t* p) {
  return static_cast<uint16_t>(p[0] | (p[1] << 8));
}
uint32_t read_le32(const uint8_t* p) {
  return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
         (static_cast<uint32_t>(p[2]) << 16) |
         (static_cast<uint32_t>(p[3]) << 24);
}
uint64_t read_le64(const uint8_t* p) {
  uint64_t v = 0;
  for (int i = 7; i >= 0; --i) v = (v << 8) | p[i];
  return v;
}

void write_be16(uint8_t* p, uint16_t v) {
  p[0] = static_cast<uint8_t>(v >> 8);
  p[1] = static_cast<uint8_t>(v);
}
void write_be32(uint8_t* p, uint32_t v) {
  p[0] = static_cast<uint8_t>(v >> 24);
  p[1] = static_cast<uint8_t>(v >> 16);
  p[2] = static_cast<uint8_t>(v >> 8);
  p[3] = static_cast<uint8_t>(v);
}
uint16_t read_be16(const uint8_t* p) {
  return static_cast<uint16_t>((p[0] << 8) | p[1]);
}
uint32_t read_be32(const uint8_t* p) {
  return (static_cast<uint32_t>(p[0]) << 24) |
         (static_cast<uint32_t>(p[1]) << 16) |
         (static_cast<uint32_t>(p[2]) << 8) | static_cast<uint32_t>(p[3]);
}

uint64_t mono_us() {
  using clock = std::chrono::steady_clock;
  return static_cast<uint64_t>(
      std::chrono::duration_cast<std::chrono::microseconds>(
          clock::now().time_since_epoch())
          .count());
}
uint64_t mono_ms() { return mono_us() / 1000; }

std::string hexdump(const uint8_t* data, size_t len, size_t max_bytes) {
  std::ostringstream os;
  const size_t n = len < max_bytes ? len : max_bytes;
  for (size_t i = 0; i < n; i += 16) {
    char line[128];
    std::snprintf(line, sizeof(line), "%08zx  ", i);
    os << line;
    for (size_t j = 0; j < 16; ++j) {
      if (i + j < n) {
        char b[4];
        std::snprintf(b, sizeof(b), "%02x ", data[i + j]);
        os << b;
      } else {
        os << "   ";
      }
      if (j == 7) os << ' ';
    }
    os << " |";
    for (size_t j = 0; j < 16 && i + j < n; ++j) {
      const uint8_t c = data[i + j];
      os << (c >= 0x20 && c < 0x7f ? static_cast<char>(c) : '.');
    }
    os << "|\n";
  }
  if (len > n) os << "... (" << (len - n) << " more bytes)\n";
  return os.str();
}

std::string to_hex(const uint8_t* data, size_t len) {
  static const char* kHex = "0123456789abcdef";
  std::string out;
  out.reserve(len * 2);
  for (size_t i = 0; i < len; ++i) {
    out.push_back(kHex[data[i] >> 4]);
    out.push_back(kHex[data[i] & 0xf]);
  }
  return out;
}

bool parse_u64(const std::string& text, uint64_t* out) {
  if (text.empty()) return false;
  char* end = nullptr;
  errno = 0;
  const unsigned long long v = std::strtoull(text.c_str(), &end, 0);
  if (errno != 0 || end == text.c_str() || *end != '\0') return false;
  *out = static_cast<uint64_t>(v);
  return true;
}

bool parse_int(const std::string& text, int* out) {
  if (text.empty()) return false;
  char* end = nullptr;
  errno = 0;
  const long v = std::strtol(text.c_str(), &end, 0);
  if (errno != 0 || end == text.c_str() || *end != '\0') return false;
  *out = static_cast<int>(v);
  return true;
}

std::string trim(const std::string& s) {
  const auto b = s.find_first_not_of(" \t\r\n");
  if (b == std::string::npos) return "";
  const auto e = s.find_last_not_of(" \t\r\n");
  return s.substr(b, e - b + 1);
}

bool read_file(const std::string& path, std::vector<uint8_t>* out,
               std::string* err) {
  std::ifstream f(path, std::ios::binary);
  if (!f) {
    if (err) *err = "cannot open file: " + path;
    return false;
  }
  f.seekg(0, std::ios::end);
  const std::streamoff size = f.tellg();
  if (size < 0) {
    // 无法定位的伪文件（procfs 与 sysfs）改为顺序读取。
    f.clear();
    f.seekg(0, std::ios::beg);
    std::ostringstream stream;
    stream << f.rdbuf();
    const std::string text = stream.str();
    out->assign(text.begin(), text.end());
    return true;
  }
  f.seekg(0, std::ios::beg);
  out->resize(static_cast<size_t>(size));
  if (size > 0) {
    f.read(reinterpret_cast<char*>(out->data()), size);
    if (!f) {
      if (err) *err = "short read: " + path;
      return false;
    }
  }
  return true;
}

bool write_file(const std::string& path, const void* data, size_t len,
                std::string* err) {
  std::ofstream f(path, std::ios::binary | std::ios::trunc);
  if (!f) {
    if (err) *err = "cannot create file: " + path;
    return false;
  }
  f.write(reinterpret_cast<const char*>(data), static_cast<std::streamsize>(len));
  if (!f) {
    if (err) *err = "write failed: " + path;
    return false;
  }
  return true;
}

}  // namespace usbdisplay
