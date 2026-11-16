// SPDX-License-Identifier: MIT
// Network interface / reachability helpers for diagnostics (getifaddrs).
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace usbdisplay {
namespace transport {

struct NetInterface {
  std::string name;
  bool is_up = false;
  uint16_t mtu = 0;
  std::string mac;
  std::vector<std::string> ipv4;
  std::vector<std::string> ipv6;
};

std::vector<NetInterface> enumerate_net_interfaces(std::string* err);

// True when the address belongs to one of the known USB Display receiver
// subnets (192.168.203.0/24 or 192.168.168.0/24).
bool ip_in_device_subnet(const std::string& ip);

// Non-blocking TCP connect probe.
bool tcp_probe(const std::string& host, uint16_t port, int timeout_ms);

}  // namespace transport
}  // namespace usbdisplay
