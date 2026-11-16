// SPDX-License-Identifier: MIT
// Process-wide logging/trace facilities. Tools set the level from the CLI;
// the library user can replace the sink with set_trace() on Controller.
#pragma once

#include <string>

#include "usbdisplay/types.h"

namespace usbdisplay {
namespace log {

void set_level(TraceLevel level);
TraceLevel level();
void set_sink(TraceSink sink);

bool enabled(TraceLevel level);

void write(TraceLevel level, const std::string& message);
void error(const std::string& message);
void warn(const std::string& message);
void info(const std::string& message);
void debug(const std::string& message);
void wire(const std::string& message);

const char* level_name(TraceLevel level);

}  // namespace log
}  // namespace usbdisplay
