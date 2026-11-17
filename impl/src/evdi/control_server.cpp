// SPDX-License-Identifier: MIT
#include "evdi/control_server.h"
#include <fcntl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>
#include <cerrno>
#include <chrono>
#include <cstring>
namespace usbdisplay { namespace evdi {
namespace {
uint64_t now_ms() {
  return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
      std::chrono::steady_clock::now().time_since_epoch()).count());
}
}
ControlServer::ControlServer(ConnectFn connect, DisconnectFn disconnect,
                             StatusFn status)
    : connect_(std::move(connect)), disconnect_(std::move(disconnect)),
      status_(std::move(status)) {}
ControlServer::~ControlServer() { close(); }
bool ControlServer::listen(const std::string& path, std::string* err) {
  close();
  path_ = path;
  listen_fd_ = ::socket(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
  if (listen_fd_ < 0) { if (err) *err = std::strerror(errno); return false; }
  ::unlink(path.c_str());
  sockaddr_un address{};
  address.sun_family = AF_UNIX;
  if (path.size() >= sizeof(address.sun_path)) {
    if (err) *err = "control socket 路径过长";
    close();
    return false;
  }
  std::memcpy(address.sun_path, path.c_str(), path.size() + 1);
  if (::bind(listen_fd_, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0 ||
      ::chmod(path.c_str(), 0660) != 0 || ::listen(listen_fd_, 8) != 0) {
    if (err) *err = std::strerror(errno);
    close();
    return false;
  }
  return true;
}
void ControlServer::reply(Client* client, const std::string& line) {
  client->output += line;
  client->output += '\n';
  size_t sent = 0;
  while (sent < client->output.size()) {
    const ssize_t n = ::send(client->fd, client->output.data() + sent,
                             client->output.size() - sent, MSG_NOSIGNAL);
    if (n > 0) { sent += static_cast<size_t>(n); continue; }
    if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)) break;
    client->fd = -1;
    return;
  }
  if (sent > 0) client->output.erase(0, sent);
  if (client->output.size() > 65536) client->fd = -1;
}
void ControlServer::process(Client* client) {
  size_t newline = 0;
  while ((newline = client->input.find('\n')) != std::string::npos) {
    std::string line = client->input.substr(0, newline);
    client->input.erase(0, newline + 1);
    if (!line.empty() && line.back() == '\r') line.pop_back();
    if (line == "CONNECT" || line.rfind("CONNECT ", 0) == 0) {
      int fps = 60;
      if (line.size() > 8) {
        const std::string value = line.substr(8);
        if (value == "60") fps = 60;
        else if (value == "120") fps = 120;
        else { reply(client, "ERR unsupported-fps"); continue; }
      }
      std::string reason;
      if (connect_ && connect_(fps, &reason)) {
        reply(client, "OK connected=1");
      } else {
        reply(client, "ERR " + (reason.empty() ? "connect failed" : reason));
      }
    } else if (line == "DISCONNECT") {
      if (disconnect_) disconnect_();
      reply(client, "OK connected=0");
    } else if (line == "STATUS") {
      reply(client, status_ ? status_() : "OK connected=0 width=1920 height=1080");
    } else {
      reply(client, "ERR unknown");
    }
    if (client->fd < 0) return;
  }
  if (client->input.size() > 4096) client->fd = -1;
}
void ControlServer::close_client(size_t index) {
  if (clients_[index].fd >= 0) ::close(clients_[index].fd);
  clients_[index].fd = -1;
}
void ControlServer::poll() {
  if (listen_fd_ < 0) return;
  for (int i = 0; i < 4; ++i) {
    const int fd = ::accept4(listen_fd_, nullptr, nullptr, SOCK_NONBLOCK | SOCK_CLOEXEC);
    if (fd < 0) break;
    Client client;
    client.fd = fd;
    client.last_activity_ms = now_ms();
    if (clients_.size() >= 8) { ::close(fd); continue; }
    clients_.push_back(std::move(client));
  }
  const uint64_t now = now_ms();
  for (size_t i = 0; i < clients_.size(); ++i) {
    Client& client = clients_[i];
    if (client.fd < 0) continue;
    if (now - client.last_activity_ms > 5000) { close_client(i); continue; }
    for (int rounds = 0; rounds < 4; ++rounds) {
      char buffer[256];
      const ssize_t got = ::recv(client.fd, buffer, sizeof(buffer), 0);
      if (got == 0) { close_client(i); break; }
      if (got < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) break;
        close_client(i);
        break;
      }
      client.input.append(buffer, static_cast<size_t>(got));
      client.last_activity_ms = now;
      process(&client);
      if (client.fd < 0) break;
    }
  }
  for (size_t i = clients_.size(); i > 0; --i) {
    if (clients_[i - 1].fd < 0) clients_.erase(clients_.begin() + (i - 1));
  }
}
void ControlServer::close() {
  for (auto& client : clients_) if (client.fd >= 0) ::close(client.fd);
  clients_.clear();
  if (listen_fd_ >= 0) { ::close(listen_fd_); listen_fd_ = -1; }
  if (!path_.empty()) { ::unlink(path_.c_str()); path_.clear(); }
}
bool ControlServer::listening() const { return listen_fd_ >= 0; }
}}  // namespace
