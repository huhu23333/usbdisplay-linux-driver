// SPDX-License-Identifier: MIT
#include "evdi/frame_lease_server.h"
#include <fcntl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>
#include <cerrno>
#include <chrono>
#include <cstring>
namespace usbdisplay { namespace evdi {
FrameLeaseServer::FrameLeaseServer(SharedFrameWriter* writer) : writer_(writer) {}
FrameLeaseServer::~FrameLeaseServer() { close(); }
bool FrameLeaseServer::listen(const std::string& path, std::string* err) {
  close();
  path_ = path;
  listen_fd_ = ::socket(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
  if (listen_fd_ < 0) { if (err) *err = std::strerror(errno); return false; }
  ::unlink(path.c_str());
  sockaddr_un address{};
  address.sun_family = AF_UNIX;
  if (path.size() >= sizeof(address.sun_path)) {
    if (err) *err = "socket 路径过长";
    close();
    return false;
  }
  std::memcpy(address.sun_path, path.c_str(), path.size() + 1);
  if (::bind(listen_fd_, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0 ||
      ::chmod(path.c_str(), 0660) != 0 || ::listen(listen_fd_, 1) != 0) {
    if (err) *err = std::strerror(errno);
    close();
    return false;
  }
  return true;
}
static uint64_t server_now_ms() {
  return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
      std::chrono::steady_clock::now().time_since_epoch()).count());
}
void FrameLeaseServer::set_timeouts(int idle_ms, int half_ms, int lease_ms) {
  idle_timeout_ms_ = idle_ms; half_packet_timeout_ms_ = half_ms; lease_timeout_ms_ = lease_ms;
}
size_t FrameLeaseServer::pending_output() const { return output_.size(); }
void FrameLeaseServer::check_timeouts(uint64_t now_ms) {
  if (client_fd_ < 0) return;
  // 租约持有期间不因空闲/半包/租约时长主动关闭：reader 复制 8MB 时可能
  // 长时间没有 socket 消息，释放 READING 槽会让 writer 覆盖正在复制的帧。
  // 客户端断开（EOF/错误）才释放；异常挂死由 consumer 进程/服务重启处理。
  if (lease_active_) {
    if (output_.size() > 65536) close_client();
    return;
  }
  const int timeout = input_size_ > 0 ? half_packet_timeout_ms_ : idle_timeout_ms_;
  if (last_activity_ms_ != 0 &&
      now_ms - last_activity_ms_ > static_cast<uint64_t>(timeout)) {
    close_client();
    return;
  }
  if (output_.size() > 65536) close_client();
}
void FrameLeaseServer::flush_output() {
  if (client_fd_ < 0) return;
  size_t sent = 0;
  while (sent < output_.size()) {
    const ssize_t n = ::send(client_fd_, output_.data() + sent,
                             output_.size() - sent, MSG_NOSIGNAL);
    if (n > 0) { sent += static_cast<size_t>(n); continue; }
    if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)) break;
    close_client();
    return;
  }
  if (sent > 0) output_.erase(output_.begin(), output_.begin() + sent);
}
void FrameLeaseServer::close_client() {
  if (lease_active_ && writer_) {
    writer_->lease_release(lease_token_);
    lease_active_ = false;
    lease_token_ = 0;
  }
  if (client_fd_ >= 0) { ::close(client_fd_); client_fd_ = -1; }
  input_size_ = 0;
  output_.clear();
  lease_started_ms_ = 0;
}
void FrameLeaseServer::poll() {
  if (listen_fd_ < 0) return;
  const uint64_t now_ms = server_now_ms();
  check_timeouts(now_ms);
  flush_output();
  if (client_fd_ < 0) {
    const int fd = ::accept4(listen_fd_, nullptr, nullptr, SOCK_NONBLOCK | SOCK_CLOEXEC);
    if (fd >= 0) {
      client_fd_ = fd;
      last_activity_ms_ = now_ms;
    }
  }
  if (client_fd_ < 0) return;
  for (int rounds = 0; rounds < 4; ++rounds) {
    const ssize_t got = ::recv(client_fd_, input_ + input_size_,
                               kLeaseRequestSize - input_size_, 0);
    if (got == 0) { close_client(); return; }
    if (got < 0) {
      if (errno == EAGAIN || errno == EWOULDBLOCK) break;
      close_client(); return;
    }
    input_size_ += static_cast<size_t>(got);
    last_activity_ms_ = now_ms;
    while (input_size_ >= kLeaseRequestSize) {
      process_request(input_);
      std::memmove(input_, input_ + kLeaseRequestSize,
                   input_size_ - kLeaseRequestSize);
      input_size_ -= kLeaseRequestSize;
    }
  }
}
void FrameLeaseServer::process_request(const uint8_t* data) {
  LeaseRequest request;
  LeaseReply reply;
  if (!decode_lease_request(data, &request)) {
    reply.status = LeaseStatus::Error;
  } else if (request.command == LeaseCommand::Acquire) {
    if (lease_active_) {
      reply.status = LeaseStatus::Stale;
    } else {
      SharedLeaseInfo info;
      if (!writer_ || !writer_->lease_acquire(&info)) {
        reply.status = LeaseStatus::Empty;
      } else {
        lease_active_ = true;
        lease_token_ = info.token;
        lease_started_ms_ = server_now_ms();
        reply.status = LeaseStatus::Ok;
        reply.token = info.token;
        reply.slot = info.slot;
        reply.width = info.width;
        reply.height = info.height;
        reply.stride = info.stride;
        reply.seq = info.seq;
        reply.generation = info.generation;
        reply.offset = info.offset;
        reply.payload = info.payload;
      }
    }
  } else {
    if (lease_active_ && request.token == lease_token_ && writer_ &&
        writer_->lease_release(request.token)) {
      lease_active_ = false;
      lease_token_ = 0;
      lease_started_ms_ = 0;
      reply.status = LeaseStatus::Ok;
    } else {
      reply.status = LeaseStatus::Stale;
    }
  }
  uint8_t encoded[kLeaseReplySize];
  encode_lease_reply(reply, encoded);
  output_.insert(output_.end(), encoded, encoded + sizeof(encoded));
  flush_output();
}
void FrameLeaseServer::close() {
  close_client();
  if (listen_fd_ >= 0) { ::close(listen_fd_); listen_fd_ = -1; }
  if (!path_.empty()) { ::unlink(path_.c_str()); path_.clear(); }
}
bool FrameLeaseServer::listening() const { return listen_fd_ >= 0; }
}}  // namespace
