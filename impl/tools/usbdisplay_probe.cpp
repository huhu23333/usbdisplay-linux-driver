// SPDX-License-Identifier: MIT
// usbdisplay_probe - inspect the receiver's HID input reports and (optionally)
// send one carefully specified report/message. Read-only by default; it never
// sends anything unless --send-json/--send-hex is given.
//
//   usbdisplay_probe                          # read both interfaces for 5 s
//   usbdisplay_probe --seconds 10 --hex 64
//   usbdisplay_probe --interface control --send-json '{"jsonrpc":"2.0",...}'
//
// Useful when comparing the device's own replies with the Windows app: run
// this while the app is active, or after sending candidate init messages.
#include <poll.h>
#include <unistd.h>

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "common/log.h"
#include "common/util.h"
#include "hid/hid_profile.h"
#include "hid/report_sink.h"
#include "transport/hidraw.h"

namespace {

struct Options {
  std::string control_path;
  std::string video_path;
  std::string interface = "both";  // control|video|both
  std::string send_json;
  std::string send_hex;
  int seconds = 5;
  size_t control_report_size = 4096;
  size_t video_report_size = 12288;
  uint8_t control_report_id = 1;
  uint8_t video_report_id = 2;
  size_t hex_bytes = 64;
  bool header_per_chunk = false;
};

void print_usage() {
  std::printf(
      "usage: usbdisplay_probe [options]\n"
      "  --interface control|video|both  which HID interface to watch\n"
      "  --control-hidraw PATH           IF0 node (default auto)\n"
      "  --video-hidraw PATH             IF1 node (default auto)\n"
      "  --seconds N                     read duration (default 5)\n"
      "  --hex N                         bytes to hexdump per report (default 64)\n"
      "  --control-report-size N         default 4096\n"
      "  --video-report-size N           default 12288\n"
      "  --send-json JSON                send this JSON on the control channel\n"
      "  --send-hex HEXBYTES             send raw payload bytes on the control channel\n"
      "  --header-per-chunk              repeat the outer header per report\n");
}

bool parse_hex(const std::string& text, std::vector<uint8_t>* out,
               std::string* err) {
  std::string clean;
  for (char c : text) {
    if (c == ' ' || c == ':' || c == '-') continue;
    clean.push_back(c);
  }
  if (clean.size() % 2 != 0) {
    if (err) *err = "hex string has an odd number of digits";
    return false;
  }
  auto nibble = [](char c, bool* ok) -> int {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    *ok = false;
    return 0;
  };
  out->clear();
  for (size_t i = 0; i < clean.size(); i += 2) {
    bool ok = true;
    const int hi = nibble(clean[i], &ok);
    const int lo = nibble(clean[i + 1], &ok);
    if (!ok) {
      if (err) *err = "invalid hex digit";
      return false;
    }
    out->push_back(static_cast<uint8_t>((hi << 4) | lo));
  }
  return true;
}

bool resolve_paths(Options* options, std::string* err) {
  if (options->interface != "control" && options->interface != "video" &&
      options->interface != "both") {
    if (err) *err = "invalid --interface";
    return false;
  }
  usbdisplay::transport::HidrawInfo info;
  if (options->control_path.empty() &&
      options->interface != "video") {
    if (usbdisplay::transport::find_hidraw_by_interface(
            usbdisplay::kDefaultVid, usbdisplay::kDefaultPid, 0, &info, err)) {
      options->control_path = info.path;
    } else {
      return false;
    }
  }
  if (options->video_path.empty() && options->interface != "control") {
    if (usbdisplay::transport::find_hidraw_by_interface(
            usbdisplay::kDefaultVid, usbdisplay::kDefaultPid, 1, &info, err)) {
      options->video_path = info.path;
    } else {
      return false;
    }
  }
  return true;
}

void describe_input_report(const uint8_t* report, size_t len, size_t hex_bytes) {
  const uint8_t report_id = len > 0 ? report[0] : 0;
  const uint8_t* data = report + (len > 0 ? 1 : 0);
  size_t data_len = len > 0 ? len - 1 : 0;
  std::printf("  report id=%u len=%zu\n", report_id, len);
  // Many device replies are plain JSON.
  if (data_len > 0 && (data[0] == '{' || data[0] == '[')) {
    size_t end = data_len;
    while (end > 0 && data[end - 1] == 0) --end;
    std::printf("  json: %.*s\n", static_cast<int>(end > 512 ? 512 : end),
                reinterpret_cast<const char*>(data));
    return;
  }
  if (len >= usbdisplay::hid::kOuterHeaderSize) {
    usbdisplay::hid::OuterHeader outer;
    std::string err;
    if (usbdisplay::hid::parse_outer_header(report, len, &outer, &err)) {
      std::printf("  outer header: len_field=%u param3=0x%08x "
                  "param4=0x%08x\n",
                  outer.length, outer.param3, outer.param4);
      const uint8_t* payload =
          report + usbdisplay::hid::kOuterHeaderSize;
      const size_t payload_len = len - usbdisplay::hid::kOuterHeaderSize;
      if (payload_len >= usbdisplay::hid::kInnerHeaderSize) {
        usbdisplay::hid::InnerHeader inner;
        if (usbdisplay::hid::parse_inner_header(payload, payload_len, &inner,
                                                 &err)) {
          std::printf("  inner header: seq=%u len=%u type=0x%x magic=0x%04x "
                      "one=%u a=%u b=%u c=%u\n",
                      inner.seq, inner.length, inner.msg_type, inner.magic,
                      inner.one, inner.a, inner.b, inner.c);
        }
      }
    }
  }
  std::printf("%s", usbdisplay::hexdump(report, len, hex_bytes).c_str());
}

bool send_payload(const std::string& path, uint8_t report_id,
                  size_t report_size, const std::vector<uint8_t>& payload,
                  bool header_per_chunk) {
  usbdisplay::transport::HidrawDevice device;
  std::string err;
  if (!device.open(path, &err)) {
    std::fprintf(stderr, "open %s: %s\n", path.c_str(), err.c_str());
    return false;
  }
  std::vector<std::vector<uint8_t>> reports;
  if (!usbdisplay::hid::build_reports(
          payload.data(), payload.size(), report_id, report_size,
          header_per_chunk, 0, 0, &reports, &err)) {
    std::fprintf(stderr, "build reports: %s\n", err.c_str());
    return false;
  }
  for (const auto& report : reports) {
    std::printf("TX %zu bytes:\n%s", report.size(),
                usbdisplay::hexdump(report.data(), report.size(), 64).c_str());
    if (!device.write_raw(report.data(), report.size(), &err)) {
      std::fprintf(stderr, "write: %s\n", err.c_str());
      return false;
    }
  }
  return true;
}

struct Target {
  std::string path;
  size_t report_size = 0;
  uint8_t report_id = 0;
  usbdisplay::transport::HidrawDevice device;
  uint64_t reports = 0;
  uint64_t bytes = 0;
};

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
    } else if (arg == "--video-hidraw") {
      if (!next(&options.video_path)) return 2;
    } else if (arg == "--interface") {
      if (!next(&options.interface)) return 2;
    } else if (arg == "--seconds") {
      uint64_t v = 0;
      if (!number(&v)) return 2;
      options.seconds = static_cast<int>(v);
    } else if (arg == "--hex") {
      uint64_t v = 0;
      if (!number(&v)) return 2;
      options.hex_bytes = static_cast<size_t>(v);
    } else if (arg == "--control-report-size") {
      uint64_t v = 0;
      if (!number(&v)) return 2;
      options.control_report_size = static_cast<size_t>(v);
    } else if (arg == "--video-report-size") {
      uint64_t v = 0;
      if (!number(&v)) return 2;
      options.video_report_size = static_cast<size_t>(v);
    } else if (arg == "--send-json") {
      if (!next(&options.send_json)) return 2;
    } else if (arg == "--send-hex") {
      if (!next(&options.send_hex)) return 2;
    } else if (arg == "--header-per-chunk") {
      options.header_per_chunk = true;
    } else if (arg == "--help" || arg == "-h") {
      print_usage();
      return 0;
    } else {
      std::fprintf(stderr, "unknown argument: %s\n", arg.c_str());
      print_usage();
      return 2;
    }
  }
  log::set_level(TraceLevel::Warn);
  std::string err;
  if (!resolve_paths(&options, &err)) {
    std::fprintf(stderr, "device discovery failed: %s\n", err.c_str());
    return 1;
  }

  if (!options.send_json.empty() || !options.send_hex.empty()) {
    if (options.control_path.empty()) {
      std::fprintf(stderr, "no control hidraw node for sending\n");
      return 1;
    }
    std::vector<uint8_t> payload;
    if (!options.send_hex.empty()) {
      if (!parse_hex(options.send_hex, &payload, &err)) {
        std::fprintf(stderr, "%s\n", err.c_str());
        return 2;
      }
    } else {
      payload.assign(options.send_json.begin(), options.send_json.end());
    }
    std::printf("sending %zu bytes on control channel %s\n", payload.size(),
                options.control_path.c_str());
    if (!send_payload(options.control_path, options.control_report_id,
                      options.control_report_size, payload,
                      options.header_per_chunk)) {
      return 1;
    }
  }

  // HidrawDevice is not movable, so keep targets behind unique_ptr.
  std::vector<std::unique_ptr<Target>> targets;
  if (options.interface != "video" && !options.control_path.empty()) {
    std::unique_ptr<Target> target(new Target());
    target->path = options.control_path;
    target->report_size = options.control_report_size;
    target->report_id = options.control_report_id;
    targets.push_back(std::move(target));
  }
  if (options.interface != "control" && !options.video_path.empty()) {
    std::unique_ptr<Target> target(new Target());
    target->path = options.video_path;
    target->report_size = options.video_report_size;
    target->report_id = options.video_report_id;
    targets.push_back(std::move(target));
  }
  if (targets.empty()) {
    std::fprintf(stderr, "no HID interfaces selected/found\n");
    return 1;
  }
  std::vector<struct pollfd> fds;
  for (auto& target_ptr : targets) {
    Target& target = *target_ptr;
    if (!target.device.open(target.path, &err)) {
      std::fprintf(stderr, "open %s: %s\n", target.path.c_str(), err.c_str());
      return 1;
    }
    std::printf("watching %s (report id %u, size %zu)\n", target.path.c_str(),
                target.report_id, target.report_size);
    struct pollfd pfd{};
    pfd.fd = target.device.fd();
    pfd.events = POLLIN;
    fds.push_back(pfd);
  }
  // Also watch for stdin EOF/control? Keep it simple: time based.
  const uint64_t start_ms = mono_ms();
  std::vector<uint8_t> buffer(16384);
  while ((mono_ms() - start_ms) < static_cast<uint64_t>(options.seconds) * 1000) {
    const int rc = ::poll(fds.data(), fds.size(), 200);
    if (rc <= 0) continue;
    for (size_t i = 0; i < fds.size(); ++i) {
      if ((fds[i].revents & POLLIN) == 0) continue;
      const int n = targets[i]->device.read_report(buffer.data(), buffer.size(),
                                                   0, &err);
      if (n < 0) {
        std::fprintf(stderr, "read %s: %s\n", targets[i]->path.c_str(),
                     err.c_str());
        continue;
      }
      if (n == 0) continue;
      ++targets[i]->reports;
      targets[i]->bytes += static_cast<size_t>(n);
      std::printf("[%s] input report:\n", targets[i]->path.c_str());
      describe_input_report(buffer.data(), static_cast<size_t>(n),
                            options.hex_bytes);
    }
  }
  std::printf("\nsummary:\n");
  for (const auto& target_ptr : targets) {
    std::printf("  %s: %llu input reports, %llu bytes\n",
                target_ptr->path.c_str(),
                static_cast<unsigned long long>(target_ptr->reports),
                static_cast<unsigned long long>(target_ptr->bytes));
  }
  return 0;
}
