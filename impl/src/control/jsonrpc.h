// SPDX-License-Identifier: MIT
// JSON-RPC 2.0 helpers for the receiver control channel.
#pragma once

#include <cstdint>
#include <string>

#include "common/json.h"
#include "control/control_channel.h"

namespace usbdisplay {
namespace control {

struct RpcMessage {
  bool is_response = false;
  bool has_id = false;
  int id = 0;
  bool has_error = false;
  int error_code = 0;
  std::string error_message;
  json::Value result;
  std::string method;  // for notifications / requests from the receiver
  json::Value params;
  std::string raw;
};

class JsonRpcClient {
 public:
  explicit JsonRpcClient(ControlChannel& channel) : channel_(channel) {}

  int next_id() { return ++id_; }

  bool notify(const std::string& method, const json::Value& params,
              std::string* err);
  bool call(const std::string& method, const json::Value& params,
            json::Value* result, std::string* err);

  // Wait for one message; timeout returns false with timed_out=true.
  bool recv(RpcMessage* message, int timeout_ms, bool* timed_out,
            std::string* err);

  static std::string make_request(const std::string& method,
                                  const json::Value& params, int id);
  static std::string make_notification(const std::string& method,
                                       const json::Value& params);
  static bool parse(const std::string& text, RpcMessage* out, std::string* err);

 private:
  ControlChannel& channel_;
  int id_ = 0;
};

}  // namespace control
}  // namespace usbdisplay
