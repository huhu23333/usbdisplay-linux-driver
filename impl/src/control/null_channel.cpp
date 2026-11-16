// SPDX-License-Identifier: MIT
#include "control/null_channel.h"

#include "common/log.h"

namespace usbdisplay {
namespace control {

bool NullControlChannel::send_json(const std::string& json, std::string*) {
  log::debug("null-control tx: " + json);
  return true;
}

bool NullControlChannel::recv_json(std::string*, int, bool* timed_out,
                                   std::string*) {
  if (timed_out) *timed_out = true;
  return false;
}

}  // namespace control
}  // namespace usbdisplay
