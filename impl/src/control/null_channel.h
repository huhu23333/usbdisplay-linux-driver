// SPDX-License-Identifier: MIT
// Control channel used for --dry-run / offline builds: sends are accepted and
// logged, receives always time out.
#pragma once

#include "control/control_channel.h"

namespace usbdisplay {
namespace control {

class NullControlChannel : public ControlChannel {
 public:
  bool open(std::string*) override {
    open_ = true;
    return true;
  }
  void close() override { open_ = false; }
  bool connected() const override { return open_; }
  bool send_json(const std::string& json, std::string*) override;
  bool recv_json(std::string*, int, bool* timed_out, std::string*) override;
  std::string describe() const override { return "null-control"; }

 private:
  bool open_ = false;
};

}  // namespace control
}  // namespace usbdisplay
