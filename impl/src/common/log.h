// SPDX-License-Identifier: MIT
// 进程级日志输出。工具通过命令行设置级别，
// 库使用方可通过 Controller::set_trace 替换输出函数。
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
