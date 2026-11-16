// SPDX-License-Identifier: MIT
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace usbdisplay {

void write_le16(uint8_t* p, uint16_t v);
void write_le32(uint8_t* p, uint32_t v);
void write_le64(uint8_t* p, uint64_t v);
uint16_t read_le16(const uint8_t* p);
uint32_t read_le32(const uint8_t* p);
uint64_t read_le64(const uint8_t* p);

void write_be16(uint8_t* p, uint16_t v);
void write_be32(uint8_t* p, uint32_t v);
uint16_t read_be16(const uint8_t* p);
uint32_t read_be32(const uint8_t* p);

uint64_t mono_us();
uint64_t mono_ms();

std::string hexdump(const uint8_t* data, size_t len, size_t max_bytes = 64);
std::string to_hex(const uint8_t* data, size_t len);

bool parse_u64(const std::string& text, uint64_t* out);
bool parse_int(const std::string& text, int* out);
std::string trim(const std::string& s);

// Read/write whole files. err is set on failure.
bool read_file(const std::string& path, std::vector<uint8_t>* out,
               std::string* err);
bool write_file(const std::string& path, const void* data, size_t len,
                std::string* err);

}  // namespace usbdisplay
