// SPDX-License-Identifier: MIT
#include "evdi/frame_lease_client.h"
#include <fcntl.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include <cerrno>
#include <cstring>
namespace usbdisplay { namespace evdi {
namespace {
bool send_all(int fd, const uint8_t* data, size_t size) {
  size_t sent = 0;
  while (sent < size) {
    const ssize_t n = ::send(fd, data + sent, size - sent, MSG_NOSIGNAL);
    if (n <= 0) {
      if (n < 0 && errno == EINTR) continue;
      return false;
    }
    sent += static_cast<size_t>(n);
  }
  return true;
}
bool recv_all(int fd, uint8_t* data, size_t size) {
  size_t got = 0;
  while (got < size) {
    const ssize_t n = ::recv(fd, data + got, size - got, 0);
    if (n <= 0) {
      if (n < 0 && errno == EINTR) continue;
      return false;
    }
    got += static_cast<size_t>(n);
  }
  return true;
}
}  // namespace
FrameLeaseClient::~FrameLeaseClient() { close(); }
bool FrameLeaseClient::connect(const std::string& path, std::string* err) {
  close();
  fd_ = ::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
  if (fd_ < 0) {
    if (err) *err = std::strerror(errno);
    return false;
  }
  sockaddr_un address{};
  address.sun_family = AF_UNIX;
  if (path.size() >= sizeof(address.sun_path)) {
    if (err) *err = "socket 路径过长";
    close();
    return false;
  }
  std::memcpy(address.sun_path, path.c_str(), path.size() + 1);
  int result = ::connect(fd_, reinterpret_cast<sockaddr*>(&address), sizeof(address));
  if (result != 0 && errno == EINPROGRESS) {
    pollfd pfd{};
    pfd.fd = fd_;
    pfd.events = POLLOUT;
    if (::poll(&pfd, 1, 500) <= 0) {
      if (err) *err = "连接 socket 超时";
      close();
      return false;
    }
    int socket_error = 0;
    socklen_t length = sizeof(socket_error);
    if (getsockopt(fd_, SOL_SOCKET, SO_ERROR, &socket_error, &length) != 0 ||
        socket_error != 0) {
      if (err) *err = "连接 socket 失败";
      close();
      return false;
    }
  } else if (result != 0) {
    if (err) *err = std::strerror(errno);
    close();
    return false;
  }
  const int flags = fcntl(fd_, F_GETFL, 0);
  if (flags >= 0) fcntl(fd_, F_SETFL, flags & ~O_NONBLOCK);
  timeval timeout{};
  timeout.tv_sec = 0;
  timeout.tv_usec = 500000;
  setsockopt(fd_, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
  setsockopt(fd_, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
  return true;
}
LeaseResult FrameLeaseClient::acquire(LeaseReply* reply, std::string* err) {
  if (fd_ < 0 || reply == nullptr) {
    if (err) *err = "socket 未连接";
    return LeaseResult::Error;
  }
  LeaseRequest request;
  request.command = LeaseCommand::Acquire;
  uint8_t encoded[kLeaseRequestSize];
  encode_lease_request(request, encoded);
  if (!send_all(fd_, encoded, sizeof(encoded))) {
    if (err) *err = "发送 ACQUIRE 失败";
    close();
    return LeaseResult::Error;
  }
  uint8_t response[kLeaseReplySize];
  if (!recv_all(fd_, response, sizeof(response))) {
    if (err) *err = "读取 ACQUIRE 回复失败";
    close();
    return LeaseResult::Error;
  }
  if (!decode_lease_reply(response, reply)) {
    if (err) *err = "ACQUIRE 回复格式错误";
    close();
    return LeaseResult::Error;
  }
  if (reply->status == LeaseStatus::Empty) {
    if (err) *err = "没有可读帧";
    return LeaseResult::Empty;
  }
  if (reply->status != LeaseStatus::Ok) {
    if (err) *err = "租约被拒绝";
    close();
    return LeaseResult::Error;
  }
  return LeaseResult::Ok;
}
bool FrameLeaseClient::release(uint64_t token, std::string* err) {
  if (fd_ < 0) {
    if (err) *err = "socket 未连接";
    return false;
  }
  LeaseRequest request;
  request.command = LeaseCommand::Release;
  request.token = token;
  uint8_t encoded[kLeaseRequestSize];
  encode_lease_request(request, encoded);
  if (!send_all(fd_, encoded, sizeof(encoded))) {
    if (err) *err = "发送 RELEASE 失败";
    close();
    return false;
  }
  uint8_t response[kLeaseReplySize];
  if (!recv_all(fd_, response, sizeof(response))) {
    if (err) *err = "读取 RELEASE 回复失败";
    close();
    return false;
  }
  LeaseReply reply;
  if (!decode_lease_reply(response, &reply)) {
    if (err) *err = "RELEASE 回复格式错误";
    close();
    return false;
  }
  if (reply.status != LeaseStatus::Ok) {
    if (err) *err = "RELEASE 被拒绝";
    close();
    return false;
  }
  return true;
}
bool FrameLeaseClient::connected() const { return fd_ >= 0; }
void FrameLeaseClient::close() { if (fd_ >= 0) { ::close(fd_); fd_ = -1; } }
}}  // namespace
