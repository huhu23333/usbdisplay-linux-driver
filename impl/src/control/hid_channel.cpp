// SPDX-License-Identifier: MIT
#include "control/hid_channel.h"

#include <algorithm>
#include <cstring>

#include "common/log.h"
#include "common/util.h"

namespace usbdisplay {
namespace control {

HidControlChannel::HidControlChannel(HidChannelConfig config)
    : config_(std::move(config)) {}
HidControlChannel::~HidControlChannel() { close(); }

bool HidControlChannel::open(std::string* err) {
  std::string path = config_.path;
  if (path.empty()) {
    transport::HidrawInfo found;
    if (!transport::find_hidraw(config_.vid, config_.pid, &found, err)) {
      return false;
    }
    path = found.path;
  }
  if (!device_.open(path, err)) return false;
  device_.get_info(&info_, nullptr);
  log::info("hid: opened " + path + " (" + info_.name + ") report_size=" +
            std::to_string(config_.report_size) +
            " report_id=" + std::to_string(config_.report_id));
  return true;
}

void HidControlChannel::close() { device_.close(); }
bool HidControlChannel::connected() const { return device_.is_open(); }

bool HidControlChannel::send_json(const std::string& json, std::string* err) {
  if (!device_.is_open()) {
    if (err) *err = "hid: channel not open";
    return false;
  }
  if (!config_.chunked) {
    if (json.size() > config_.report_size) {
      if (err) {
        *err = "hid: message of " + std::to_string(json.size()) +
               " bytes exceeds report size " +
               std::to_string(config_.report_size) +
               " (use chunked framing)";
      }
      return false;
    }
    return device_.write_output_report(
        config_.report_id, reinterpret_cast<const uint8_t*>(json.data()),
        json.size(), config_.report_size, err);
  }
  // 分片格式。
  size_t offset = 0;
  while (offset < json.size()) {
    const size_t chunk =
        std::min(config_.report_size - 4, json.size() - offset);
    std::vector<uint8_t> body(4 + chunk);
    write_le16(body.data(), static_cast<uint16_t>(json.size()));
    write_le16(body.data() + 2, static_cast<uint16_t>(offset));
    std::memcpy(body.data() + 4, json.data() + offset, chunk);
    if (!device_.write_output_report(config_.report_id, body.data(),
                                     body.size(), config_.report_size, err)) {
      return false;
    }
    offset += chunk;
  }
  return true;
}

bool HidControlChannel::recv_json(std::string* json, int timeout_ms,
                                  bool* timed_out, std::string* err) {
  if (timed_out) *timed_out = false;
  if (!device_.is_open()) {
    if (err) *err = "hid: channel not open";
    return false;
  }
  uint8_t buffer[512];
  const int n = device_.read_report(buffer, sizeof(buffer), timeout_ms, err);
  if (n < 0) return false;
  if (n == 0) {
    if (timed_out) *timed_out = true;
    return false;
  }
  // 输入报告首字节为报告编号，末尾零填充会被去除。
  size_t start = 0;
  if (n > 1 && buffer[0] == config_.report_id) start = 1;
  size_t end = static_cast<size_t>(n);
  while (end > start && buffer[end - 1] == 0) --end;
  if (end <= start) {
    if (timed_out) *timed_out = true;
    return false;
  }
  json->assign(reinterpret_cast<char*>(buffer + start), end - start);
  return true;
}

std::string HidControlChannel::describe() const {
  return "hidraw:" + info_.path + " (" + info_.name + ")";
}

}  // namespace control
}  // namespace usbdisplay
