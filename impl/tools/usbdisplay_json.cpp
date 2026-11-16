// SPDX-License-Identifier: MIT
// 通过接收器 HID 控制通道进行 JSON 会话的工具。
//
// 按 16 字节报告头部发送 JSON-RPC 消息，定时发送携带会话编号的心跳，
// 并打印设备返回的每条回复与事件。
//
//   usbdisplay_json --seconds 15
//   usbdisplay_json --send '{"jsonrpc":"2.0","method":"app-launched","params":{...}}'
//   usbdisplay_json --send @cmd.json --heartbeat-ms 500 --trace debug
//   usbdisplay_json --stdin            每行输入一条 JSON 消息
#include <poll.h>
#include <unistd.h>

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "common/json.h"
#include "common/log.h"
#include "common/util.h"
#include "hid/hid_profile.h"
#include "hid/report_sink.h"
#include "transport/hidraw.h"

namespace {

struct Options {
  std::string control_path;
  std::vector<std::string> commands;
  std::string record_path;
  std::string session_id;  // numeric string; generated when empty
  int seconds = 30;
  int heartbeat_ms = 1000;
  size_t report_size = 4096;
  uint8_t report_id = 1;
  size_t endpoint_hex = 0;
  bool stdin_mode = false;
  bool answer_requests = true;
  bool quiet = false;
  usbdisplay::TraceLevel trace = usbdisplay::TraceLevel::Info;
};

void print_usage() {
  std::printf(
      "usage: usbdisplay_json [options]\n"
      "  --control-hidraw PATH  IF0 node (default: auto interface 0)\n"
      "  --send JSON            JSON message to send at start (repeatable,\n"
      "                         @FILE reads the message from a file)\n"
      "  --stdin                send one JSON message per stdin line\n"
      "  --seconds N            session duration (default 30, 0 = forever)\n"
      "  --heartbeat-ms N       heartbeat interval (default 1000, 0 = off)\n"
      "  --report-size N        control report size (default 4096)\n"
      "  --report-id N          control report id (default 1)\n"
      "  --record FILE          append all received JSON payloads to FILE\n"
      "  --session-id ID        numeric heartbeat session id (default: 6 digits)\n"
      "  --no-answer            do not auto-answer device requests that carry id\n"
      "  --quiet                only print received messages\n"
      "  --trace LEVEL          off|error|warn|info|debug|wire\n");
}

bool parse_trace(const std::string& text, usbdisplay::TraceLevel* out) {
  static const char* names[] = {"off", "error", "warn", "info", "debug", "wire"};
  for (int i = 0; i < 6; ++i) {
    if (text == names[i]) {
      *out = static_cast<usbdisplay::TraceLevel>(i);
      return true;
    }
  }
  return false;
}

}  // namespace

int main(int argc, char** argv) {
  using namespace usbdisplay;
  Options options;
  for (int i = 1; i < argc; ++i) {
    const std::string arg = argv[i];
    auto next = [&](std::string* out) -> bool {
      if (i + 1 >= argc) return false;
      *out = argv[++i];
      return true;
    };
    auto number = [&](uint64_t* out) -> bool {
      std::string v;
      if (!next(&v)) return false;
      *out = std::strtoull(v.c_str(), nullptr, 0);
      return true;
    };
    if (arg == "--control-hidraw") {
      if (!next(&options.control_path)) return 2;
    } else if (arg == "--send") {
      std::string v;
      if (!next(&v)) return 2;
      options.commands.push_back(v);
    } else if (arg == "--record") {
      if (!next(&options.record_path)) return 2;
    } else if (arg == "--session-id") {
      if (!next(&options.session_id)) return 2;
    } else if (arg == "--stdin") {
      options.stdin_mode = true;
    } else if (arg == "--seconds") {
      uint64_t v = 0;
      if (!number(&v)) return 2;
      options.seconds = static_cast<int>(v);
    } else if (arg == "--heartbeat-ms") {
      uint64_t v = 0;
      if (!number(&v)) return 2;
      options.heartbeat_ms = static_cast<int>(v);
    } else if (arg == "--report-size") {
      uint64_t v = 0;
      if (!number(&v)) return 2;
      options.report_size = static_cast<size_t>(v);
    } else if (arg == "--report-id") {
      uint64_t v = 0;
      if (!number(&v)) return 2;
      options.report_id = static_cast<uint8_t>(v);
    } else if (arg == "--no-answer") {
      options.answer_requests = false;
    } else if (arg == "--quiet") {
      options.quiet = true;
    } else if (arg == "--trace") {
      std::string v;
      if (!next(&v) || !parse_trace(v, &options.trace)) return 2;
    } else if (arg == "--help" || arg == "-h") {
      print_usage();
      return 0;
    } else {
      std::fprintf(stderr, "unknown argument: %s\n", arg.c_str());
      print_usage();
      return 2;
    }
  }
  log::set_level(options.trace);

  transport::HidrawInfo info;
  std::string err;
  if (options.control_path.empty()) {
    if (!transport::find_hidraw_by_interface(kDefaultVid, kDefaultPid, 0, &info,
                                             &err)) {
      std::fprintf(stderr, "control hidraw discovery failed: %s\n", err.c_str());
      return 1;
    }
    options.control_path = info.path;
  }
  transport::HidrawDevice device;
  if (!device.open(options.control_path, &err)) {
    std::fprintf(stderr, "open %s: %s\n", options.control_path.c_str(),
                 err.c_str());
    return 1;
  }
  if (options.session_id.empty()) {
    options.session_id = std::to_string((mono_ms() % 900000ULL) + 100000ULL);
  }
  std::printf("control channel %s (report id %u, %zu bytes, session %s)\n",
              options.control_path.c_str(), options.report_id,
              options.report_size, options.session_id.c_str());

  FILE* record = nullptr;
  if (!options.record_path.empty()) {
    record = std::fopen(options.record_path.c_str(), "a");
  }

  size_t messages_sent = 0;
  size_t messages_received = 0;
  std::string rx_pending;
  uint64_t last_heartbeat = 0;
  const uint64_t start_ms = mono_ms();
  const uint64_t end_ms = options.seconds > 0
                              ? start_ms + static_cast<uint64_t>(options.seconds) * 1000
                              : 0;
  std::string stdin_pending;

  auto send_json = [&](const std::string& json, const char* tag) {
    std::vector<std::vector<uint8_t>> reports;
    if (!hid::build_reports(reinterpret_cast<const uint8_t*>(json.data()),
                            json.size(), options.report_id, options.report_size,
                            true, 0, 0, &reports, &err)) {
      std::fprintf(stderr, "build reports: %s\n", err.c_str());
      return;
    }
    for (const auto& report : reports) {
      if (!device.write_raw(report.data(), report.size(), &err)) {
        std::fprintf(stderr, "write: %s\n", err.c_str());
        return;
      }
    }
    ++messages_sent;
    if (!options.quiet) {
      std::printf(">>> [%s] %s\n", tag, json.c_str());
      std::fflush(stdout);
    }
  };

  auto handle_rx = [&](const uint8_t* report, size_t len) {
    if (len < hid::kOuterHeaderSize) return;
    const uint8_t rid = report[0];
    const size_t data_len = len;
    hid::OuterHeader outer;
    if (!hid::parse_outer_header(report, data_len, &outer, &err)) return;
    size_t take = outer.length;
    if (take > data_len - hid::kOuterHeaderSize) {
      take = data_len - hid::kOuterHeaderSize;
    }
    if (take == 0) return;
    std::string payload(
        reinterpret_cast<const char*>(report + hid::kOuterHeaderSize), take);
    ++messages_received;
    std::printf("rx rid=%u len=%zu: %s\n", rid, take,
                payload.substr(0, 1024).c_str());
    std::fflush(stdout);
    if (record != nullptr) {
      std::fprintf(record, "%s\n", payload.c_str());
      std::fflush(record);
    }
    // 回复带方法名与编号的设备请求。
    json::Value doc;
    std::string jerr;
    if (!json::Value::parse(payload, &doc, &jerr) || !doc.is_object()) return;
    const json::Value* method = doc.find("method");
    const json::Value* id = doc.find("id");
    if (!options.answer_requests || method == nullptr || !method->is_string() ||
        id == nullptr || !id->is_number()) {
      return;
    }
    json::Value response = json::Value::object();
    response.set("jsonrpc", json::Value("2.0"));
    if (method->as_string() == "projection-request-with-tx" ||
        method->as_string() == "projection-request-cancel-with-tx") {
      json::Value result = json::Value::object();
      result.set("action", json::Value("allow"));
      response.set("result", result);
    } else {
      response.set("result", json::Value::object());
    }
    response.set("id", json::Value(static_cast<int>(id->as_number())));
    send_json(response.dump(), "answer");
  };

  // 先发送命令行指定的消息。
  for (const auto& command : options.commands) {
    std::string json = command;
    if (!command.empty() && command[0] == '@') {
      std::vector<uint8_t> bytes;
      if (!read_file(command.substr(1), &bytes, &err)) {
        std::fprintf(stderr, "%s\n", err.c_str());
        continue;
      }
      json.assign(bytes.begin(), bytes.end());
    }
    send_json(json, "cmd");
  }

  std::vector<uint8_t> buffer(options.report_size + 1);
  while (end_ms == 0 || mono_ms() < end_ms) {
    const uint64_t now = mono_ms();
    if (options.heartbeat_ms > 0 &&
        now - last_heartbeat >= static_cast<uint64_t>(options.heartbeat_ms)) {
      last_heartbeat = now;
      // 心跳必须携带会话编号，设备会在回复中回传该编号。
      send_json("{\"jsonrpc\":\"2.0\", \"method\": \"heartbeat\", \"params\": "
                "{\"session-id\": \"" +
                    options.session_id + "\"}}",
                "hb");
    }
    if (options.stdin_mode) {
      // 非阻塞读取标准输入的完整 JSON 行。
      struct pollfd stdin_poll{};
      stdin_poll.fd = STDIN_FILENO;
      stdin_poll.events = POLLIN;
      if (::poll(&stdin_poll, 1, 0) > 0) {
        char chunk[1024];
        const ssize_t n = ::read(STDIN_FILENO, chunk, sizeof(chunk));
        if (n > 0) stdin_pending.append(chunk, static_cast<size_t>(n));
        size_t pos = 0;
        while (true) {
          const size_t nl = stdin_pending.find('\n', pos);
          if (nl == std::string::npos) break;
          const std::string line = trim(stdin_pending.substr(pos, nl - pos));
          pos = nl + 1;
          if (!line.empty()) send_json(line, "stdin");
        }
        if (pos > 0) stdin_pending.erase(0, pos);
      }
    }
    struct pollfd pfd{};
    pfd.fd = device.fd();
    pfd.events = POLLIN;
    const int rc = ::poll(&pfd, 1, 100);
    if (rc <= 0) continue;
    const int n = device.read_report(buffer.data(), buffer.size(), 0, &err);
    if (n < 0) {
      std::fprintf(stderr, "read: %s\n", err.c_str());
      break;
    }
    if (n > 0) handle_rx(buffer.data(), static_cast<size_t>(n));
  }
  std::printf("\nsummary: sent=%zu received=%zu\n", messages_sent,
              messages_received);
  if (record != nullptr) std::fclose(record);
  return 0;
}
