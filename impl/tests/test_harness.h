// SPDX-License-Identifier: MIT
// Tiny test harness: no external dependencies, works with ctest.
#pragma once

#include <cstdio>
#include <string>

namespace testharness {
inline int& failures() {
  static int value = 0;
  return value;
}
inline int& checks() {
  static int value = 0;
  return value;
}
inline void check(bool condition, const char* expression, const char* file,
                  int line) {
  ++checks();
  if (condition) return;
  ++failures();
  std::printf("FAIL %s:%d  %s\n", file, line, expression);
}
inline int finish(const char* name) {
  std::printf("%s: %d checks, %d failures\n", name, checks(), failures());
  return failures() == 0 ? 0 : 1;
}
}  // namespace testharness

#define CHECK(condition) \
  ::testharness::check((condition), #condition, __FILE__, __LINE__)
#define CHECK_MSG(condition, message)                                   \
  do {                                                                  \
    ::testharness::check((condition), #condition, __FILE__, __LINE__);  \
    if (!(condition)) std::printf("  context: %s\n", (message));        \
  } while (0)
