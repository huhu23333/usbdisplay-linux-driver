// SPDX-License-Identifier: MIT
#include <arpa/inet.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>

#include "common/log.h"
#include "transport/transport.h"

namespace usbdisplay {
namespace transport {
namespace {

bool resolve_addr(const std::string& host, uint16_t port, bool passive,
                  struct addrinfo** out, std::string* err) {
  struct addrinfo hints{};
  hints.ai_family = AF_UNSPEC;
  hints.ai_socktype = SOCK_STREAM;
  hints.ai_flags = passive ? AI_PASSIVE : 0;
  const std::string service = std::to_string(port);
  const int rc = ::getaddrinfo(host.empty() ? nullptr : host.c_str(),
                               service.c_str(), &hints, out);
  if (rc != 0) {
    if (err) *err = std::string("getaddrinfo(") + host + "): " + gai_strerror(rc);
    return false;
  }
  return true;
}

bool set_nonblocking(int fd) {
  const int flags = ::fcntl(fd, F_GETFL, 0);
  if (flags < 0) return false;
  return ::fcntl(fd, F_SETFL, flags | O_NONBLOCK) == 0;
}

}  // namespace

class TcpSink : public MediaSink {
 public:
  TcpSink(std::string host, uint16_t port, std::string bind_address,
          int connect_timeout_ms)
      : host_(std::move(host)),
        port_(port),
        bind_address_(std::move(bind_address)),
        timeout_ms_(connect_timeout_ms) {}
  ~TcpSink() override { close(); }

  bool open(std::string* err) override {
    close();
    struct addrinfo* list = nullptr;
    if (!resolve_addr(host_, port_, false, &list, err)) return false;
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
        hints.ai_socktype = SOCK_STREAM;
        hints.ai_flags = AI_PASSIVE;
        struct addrinfo* local = nullptr;
        if (::getaddrinfo(bind_address_.c_str(), nullptr, &hints, &local) == 0) {
          (void)::bind(fd, local->ai_addr, local->ai_addrlen);
          ::freeaddrinfo(local);
        }
      }
      if (!set_nonblocking(fd)) {
        last_errno = errno;
        ::close(fd);
        continue;
      }
      int rc = ::connect(fd, ai->ai_addr, ai->ai_addrlen);
      if (rc != 0 && errno == EINPROGRESS) {
        struct pollfd pfd{};
        pfd.fd = fd;
        pfd.events = POLLOUT;
        rc = ::poll(&pfd, 1, timeout_ms_);
        if (rc == 1) {
          int so_error = 0;
          socklen_t slen = sizeof(so_error);
          if (::getsockopt(fd, SOL_SOCKET, SO_ERROR, &so_error, &slen) != 0) {
            so_error = errno;
          }
          if (so_error == 0) {
            rc = 0;
          } else {
            last_errno = so_error;
            rc = -1;
          }
        } else if (rc == 0) {
          last_errno = ETIMEDOUT;
          rc = -1;
        }
      }
      if (rc == 0) {
        const int one = 1;
        (void)::setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
        fd_ = fd;
        break;
      }
      last_errno = last_errno != 0 ? last_errno : errno;
      ::close(fd);
    }
    ::freeaddrinfo(list);
    if (fd_ < 0) {
      if (err) {
        *err = "tcp connect " + host_ + ":" + std::to_string(port_) +
               " failed: " + std::strerror(last_errno);
      }
      return false;
    }
    log::info("tcp: connected to " + host_ + ":" + std::to_string(port_));
    return true;
  }

  bool send(const uint8_t* data, size_t len, std::string* err) override {
    if (fd_ < 0) {
      if (err) *err = "tcp: not connected";
      return false;
    }
    size_t sent = 0;
    while (sent < len) {
      const ssize_t n = ::send(fd_, data + sent, len - sent, MSG_NOSIGNAL);
      if (n > 0) {
        sent += static_cast<size_t>(n);
        continue;
      }
      if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
        struct pollfd pfd{};
        pfd.fd = fd_;
        pfd.events = POLLOUT;
        const int prc = ::poll(&pfd, 1, 2000);
        if (prc > 0) continue;
        if (err) *err = "tcp: send timeout";
        return false;
      }
      if (n < 0 && errno == EINTR) continue;
      if (err) {
        *err = std::string("tcp: send failed: ") + std::strerror(errno);
      }
      return false;
    }
    return true;
  }

  void close() override {
    if (fd_ >= 0) {
      ::close(fd_);
      fd_ = -1;
    }
  }

  bool connected() const override { return fd_ >= 0; }
  std::string describe() const override {
    return "tcp://" + host_ + ":" + std::to_string(port_);
  }
  bool datagram() const override { return false; }

 private:
  std::string host_;
  uint16_t port_;
  std::string bind_address_;
  int timeout_ms_;
  int fd_ = -1;
};

std::unique_ptr<MediaSink> make_tcp_sink(const std::string& host, uint16_t port,
                                         const std::string& bind_address,
                                         int connect_timeout_ms) {
  return std::unique_ptr<MediaSink>(
      new TcpSink(host, port, bind_address, connect_timeout_ms));
}

}  // namespace transport
}  // namespace usbdisplay
