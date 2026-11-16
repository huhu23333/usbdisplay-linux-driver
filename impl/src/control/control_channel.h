// SPDX-License-Identifier: MIT
// Control channel abstraction (JSON-RPC 2.0 over HID today, could be a TCP
// control socket for Wi-Fi mode later).
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

namespace usbdisplay {
namespace control {

class ControlChannel {
 public:
  virtual ~ControlChannel() = default;
  virtual bool open(std::string* err) = 0;
  virtual void close() = 0;
  virtual bool connected() const = 0;
  // Sends one JSON text message (the channel handles report framing).
  virtual bool send_json(const std::string& json, std::string* err) = 0;
  // Receives one JSON text message; returns false on timeout/error and sets
  // *timed_out when the timeout expired with no data.
  virtual bool recv_json(std::string* json, int timeout_ms, bool* timed_out,
                         std::string* err) = 0;
  virtual std::string describe() const = 0;
};

}  // namespace control
}  // namespace usbdisplay
