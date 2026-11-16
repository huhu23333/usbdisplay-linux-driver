// SPDX-License-Identifier: MIT
// 诊断使用的网络接口与可达性查询工具。
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

// 判断地址是否属于接收器的常见网段。
bool ip_in_device_subnet(const std::string& ip);

// 非阻塞 TCP 连接探测。
bool tcp_probe(const std::string& host, uint16_t port, int timeout_ms);

}  // namespace transport
}  // namespace usbdisplay
