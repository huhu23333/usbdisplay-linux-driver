// SPDX-License-Identifier: MIT
#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>

#include "common/log.h"
#include "transport/transport.h"

namespace usbdisplay {
namespace transport {
namespace {

bool resolve_udp(const std::string& host, uint16_t port, std::string* err,
                 struct addrinfo** out) {
  struct addrinfo hints{};
  hints.ai_family = AF_UNSPEC;
  hints.ai_socktype = SOCK_DGRAM;
  const std::string service = std::to_string(port);
  const int rc =
      ::getaddrinfo(host.empty() ? nullptr : host.c_str(), service.c_str(),
                    &hints, out);
  if (rc != 0) {
    if (err) {
      *err = "getaddrinfo(" + host + "): " + gai_strerror(rc);
    }
    return false;
  }
  return true;
}

}  // namespace

class UdpSink : public MediaSink {
 public:
  UdpSink(std::string host, uint16_t port, std::string bind_address)
      : host_(std::move(host)),
        port_(port),
        bind_address_(std::move(bind_address)) {}
  ~UdpSink() override { close(); }

  bool open(std::string* err) override {
    close();
    struct addrinfo* list = nullptr;
    if (!resolve_udp(host_, port_, err, &list)) return false;
    int last_errno = 0;
    for (struct addrinfo* ai = list; ai != nullptr; ai = ai->ai_next) {
      const int fd = ::socket(ai->ai_family, ai->ai_socktype | SOCK_CLOEXEC,
                              ai->ai_protocol);
      if (fd < 0) {
        last_errno = errno;
        continue;
      }
      if (!bind_address_.empty()) {
        struct addrinfo hints{};
        hints.ai_family = ai->ai_family;
        hints.ai_socktype = SOCK_DGRAM;
        hints.ai_flags = AI_PASSIVE;
        struct addrinfo* local = nullptr;
        if (::getaddrinfo(bind_address_.c_str(), nullptr, &hints, &local) == 0) {
          (void)::bind(fd, local->ai_addr, local->ai_addrlen);
          ::freeaddrinfo(local);
        }
      }
      if (::connect(fd, ai->ai_addr, ai->ai_addrlen) == 0) {
        fd_ = fd;
        break;
      }
      last_errno = errno;
      ::close(fd);
    }
    ::freeaddrinfo(list);
    if (fd_ < 0) {
      if (err) {
        *err = "udp connect " + host_ + ":" + std::to_string(port_) +
               " failed: " + std::strerror(last_errno);
      }
      return false;
    }
    log::info("udp: connected to " + host_ + ":" + std::to_string(port_));
    return true;
  }

  bool send(const uint8_t* data, size_t len, std::string* err) override {
    if (fd_ < 0) {
      if (err) *err = "udp: not connected";
      return false;
    }
    for (;;) {
      const ssize_t n = ::send(fd_, data, len, MSG_NOSIGNAL);
      if (n == static_cast<ssize_t>(len)) return true;
      if (n < 0 && errno == EINTR) continue;
      if (err) {
        *err = std::string("udp: send failed: ") + std::strerror(errno);
      }
      return false;
    }
  }

  void close() override {
    if (fd_ >= 0) {
      ::close(fd_);
      fd_ = -1;
    }
  }
  bool connected() const override { return fd_ >= 0; }
  std::string describe() const override {
    return "udp://" + host_ + ":" + std::to_string(port_);
  }
  bool datagram() const override { return true; }

 private:
  std::string host_;
  uint16_t port_;
  std::string bind_address_;
  int fd_ = -1;
};

std::unique_ptr<MediaSink> make_udp_sink(const std::string& host, uint16_t port,
                                         const std::string& bind_address) {
  return std::unique_ptr<MediaSink>(
      new UdpSink(host, port, bind_address));
}

}  // namespace transport
}  // namespace usbdisplay
