// SPDX-License-Identifier: MIT
#include "transport/netinfo.h"

#include <arpa/inet.h>
#include <fcntl.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <netdb.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <map>

namespace usbdisplay {
namespace transport {
namespace {

std::string read_first_line(const std::string& path) {
  std::ifstream f(path);
  std::string line;
  std::getline(f, line);
  return line;
}

bool get_port(const std::string& host, uint16_t port, struct sockaddr_storage* ss,
              socklen_t* sslen, std::string* err) {
  struct addrinfo hints{};
  hints.ai_family = AF_UNSPEC;
  hints.ai_socktype = SOCK_STREAM;
  struct addrinfo* list = nullptr;
  const std::string service = std::to_string(port);
  const int rc = ::getaddrinfo(host.c_str(), service.c_str(), &hints, &list);
  if (rc != 0) {
    if (err) *err = "getaddrinfo: " + std::string(gai_strerror(rc));
    return false;
  }
  std::memcpy(ss, list->ai_addr, list->ai_addrlen);
  *sslen = list->ai_addrlen;
  ::freeaddrinfo(list);
  return true;
}

}  // namespace

std::vector<NetInterface> enumerate_net_interfaces(std::string*) {
  std::vector<NetInterface> out;
  std::map<std::string, size_t> index;
  struct ifaddrs* list = nullptr;
  if (::getifaddrs(&list) != 0) return out;
  for (struct ifaddrs* ifa = list; ifa != nullptr; ifa = ifa->ifa_next) {
    if (ifa->ifa_name == nullptr) continue;
    const std::string name = ifa->ifa_name;
    auto it = index.find(name);
    if (it == index.end()) {
      index[name] = out.size();
      NetInterface ni;
      ni.name = name;
      ni.is_up = (ifa->ifa_flags & IFF_UP) != 0;
      ni.mtu = static_cast<uint16_t>(
          std::strtoul(read_first_line("/sys/class/net/" + name + "/mtu").c_str(),
                       nullptr, 10));
      ni.mac = read_first_line("/sys/class/net/" + name + "/address");
      out.push_back(std::move(ni));
      it = index.find(name);
    }
    NetInterface& ni = out[it->second];
    if (ifa->ifa_addr == nullptr) continue;
    char buf[INET6_ADDRSTRLEN] = {0};
    if (ifa->ifa_addr->sa_family == AF_INET) {
      const auto* sin = reinterpret_cast<const struct sockaddr_in*>(ifa->ifa_addr);
      if (::inet_ntop(AF_INET, &sin->sin_addr, buf, sizeof(buf)) != nullptr) {
        ni.ipv4.push_back(buf);
      }
    } else if (ifa->ifa_addr->sa_family == AF_INET6) {
      const auto* sin6 =
          reinterpret_cast<const struct sockaddr_in6*>(ifa->ifa_addr);
      if (::inet_ntop(AF_INET6, &sin6->sin6_addr, buf, sizeof(buf)) != nullptr) {
        ni.ipv6.push_back(buf);
      }
    }
  }
  ::freeifaddrs(list);
  return out;
}

bool ip_in_device_subnet(const std::string& ip) {
  return ip.rfind("192.168.203.", 0) == 0 || ip.rfind("192.168.168.", 0) == 0;
}

bool tcp_probe(const std::string& host, uint16_t port, int timeout_ms) {
  struct sockaddr_storage ss{};
  socklen_t sslen = 0;
  if (!get_port(host, port, &ss, &sslen, nullptr)) return false;
  const int fd = ::socket(ss.ss_family, SOCK_STREAM | SOCK_CLOEXEC, 0);
  if (fd < 0) return false;
  const int flags = ::fcntl(fd, F_GETFL, 0);
  ::fcntl(fd, F_SETFL, flags | O_NONBLOCK);
  int rc = ::connect(fd, reinterpret_cast<struct sockaddr*>(&ss), sslen);
  if (rc != 0 && errno == EINPROGRESS) {
    struct pollfd pfd{};
    pfd.fd = fd;
    pfd.events = POLLOUT;
    rc = ::poll(&pfd, 1, timeout_ms);
    if (rc == 1) {
      int so_error = 0;
      socklen_t slen = sizeof(so_error);
      ::getsockopt(fd, SOL_SOCKET, SO_ERROR, &so_error, &slen);
      rc = (so_error == 0) ? 0 : -1;
    } else {
      rc = -1;
    }
  }
  ::close(fd);
  return rc == 0;
}

}  // namespace transport
}  // namespace usbdisplay
