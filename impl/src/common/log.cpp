// SPDX-License-Identifier: MIT
#include "common/log.h"

#include <chrono>
#include <cstdio>
#include <ctime>
#include <mutex>
#include <sstream>

namespace usbdisplay {
namespace log {
namespace {

std::mutex g_mutex;
TraceLevel g_level = TraceLevel::Warn;
TraceSink g_sink;

std::string now_string() {
  using clock = std::chrono::system_clock;
  const auto now = clock::now();
  const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                      now.time_since_epoch()) %
                  1000;
  const std::time_t t = clock::to_time_t(now);
  std::tm tm{};
  localtime_r(&t, &tm);
  char buf[32];
  std::snprintf(buf, sizeof(buf), "%02d:%02d:%02d.%03d", tm.tm_hour, tm.tm_min,
                tm.tm_sec, static_cast<int>(ms.count()));
  return buf;
}

}  // namespace

const char* level_name(TraceLevel level) {
  switch (level) {
    case TraceLevel::Off: return "OFF";
    case TraceLevel::Error: return "ERR";
    case TraceLevel::Warn: return "WRN";
    case TraceLevel::Info: return "INF";
    case TraceLevel::Debug: return "DBG";
    case TraceLevel::Wire: return "WIR";
  }
  return "?";
}

void set_level(TraceLevel level) {
  std::lock_guard<std::mutex> lock(g_mutex);
  g_level = level;
}
TraceLevel level() {
  std::lock_guard<std::mutex> lock(g_mutex);
  return g_level;
}
void set_sink(TraceSink sink) {
  std::lock_guard<std::mutex> lock(g_mutex);
  g_sink = std::move(sink);
}

bool enabled(TraceLevel level) {
  return static_cast<int>(level) <= static_cast<int>(log::level());
}

void write(TraceLevel level, const std::string& message) {
  TraceSink sink;
  {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (static_cast<int>(level) > static_cast<int>(g_level)) return;
    sink = g_sink;
  }
  if (sink) {
    sink(level, message);
    return;
  }
  std::fprintf(stderr, "[%s] %-3s %s\n", now_string().c_str(),
               level_name(level), message.c_str());
}

void error(const std::string& m) { write(TraceLevel::Error, m); }
void warn(const std::string& m) { write(TraceLevel::Warn, m); }
void info(const std::string& m) { write(TraceLevel::Info, m); }
void debug(const std::string& m) { write(TraceLevel::Debug, m); }
void wire(const std::string& m) { write(TraceLevel::Wire, m); }

}  // namespace log
}  // namespace usbdisplay
