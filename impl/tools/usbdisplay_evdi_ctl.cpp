// SPDX-License-Identifier: MIT
// evdi 虚拟显示控制 CLI：connect|disconnect|status。
#include <fcntl.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <string>
namespace {
constexpr const char* kDefaultSocket = "/run/usbdisplay/control.sock";
bool send_command(const std::string& path, const std::string& command,
                  std::string* reply, std::string* err) {
  const int fd = ::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
  if (fd < 0) { if (err) *err = std::strerror(errno); return false; }
  sockaddr_un address{};
  address.sun_family = AF_UNIX;
  if (path.size() >= sizeof(address.sun_path)) {
    if (err) *err = "socket 路径过长";
    ::close(fd);
    return false;
  }
  std::memcpy(address.sun_path, path.c_str(), path.size() + 1);
  int result = ::connect(fd, reinterpret_cast<sockaddr*>(&address), sizeof(address));
  if (result != 0 && errno == EINPROGRESS) {
    pollfd pfd{fd, POLLOUT, 0};
    if (::poll(&pfd, 1, 500) <= 0) {
      if (err) *err = "连接控制 socket 超时";
      ::close(fd);
      return false;
    }
    int socket_error = 0;
    socklen_t length = sizeof(socket_error);
    if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &socket_error, &length) != 0 ||
        socket_error != 0) {
      if (err) *err = "连接控制 socket 失败";
      ::close(fd);
      return false;
    }
  } else if (result != 0) {
    if (err) *err = std::strerror(errno);
    ::close(fd);
    return false;
  }
  const std::string line = command + "\n";
  if (::send(fd, line.data(), line.size(), MSG_NOSIGNAL) !=
      static_cast<ssize_t>(line.size())) {
    if (err) *err = "发送命令失败";
    ::close(fd);
    return false;
  }
  reply->clear();
  char buffer[256];
  for (;;) {
    pollfd pfd{fd, POLLIN, 0};
    if (::poll(&pfd, 1, 500) <= 0) {
      if (err) *err = "读取控制回复超时";
      ::close(fd);
      return false;
    }
    const ssize_t got = ::recv(fd, buffer, sizeof(buffer), 0);
    if (got <= 0) {
      if (err) *err = "读取控制回复失败";
      ::close(fd);
      return false;
    }
    reply->append(buffer, static_cast<size_t>(got));
    if (reply->find('\n') != std::string::npos) break;
    if (reply->size() > 4096) {
      if (err) *err = "控制回复过长";
      ::close(fd);
      return false;
    }
  }
  ::close(fd);
  return true;
}
}  // namespace
int main(int argc, char** argv) {
  if (argc < 2) {
    std::fprintf(stderr, "usage: usbdisplay_evdi_ctl connect [60|120]|disconnect|status [--socket PATH]\n");
    return 2;
  }
  const std::string action = argv[1];
  std::string command;
  if (action == "connect") command = "CONNECT";
  else if (action == "disconnect") command = "DISCONNECT";
  else if (action == "status") command = "STATUS";
  else {
    std::fprintf(stderr, "unknown action: %s\n", action.c_str());
    return 2;
  }
  std::string socket_path = kDefaultSocket;
  bool fps_seen = false;
  for (int i = 2; i < argc; ++i) {
    const std::string arg = argv[i];
    if (arg == "--socket" && i + 1 < argc) {
      socket_path = argv[++i];
    } else if (action == "connect" && !fps_seen &&
               (arg == "60" || arg == "120")) {
      command += " " + arg;
      fps_seen = true;
    } else {
      std::fprintf(stderr, "unknown argument: %s\n", arg.c_str());
      return 2;
    }
  }
  std::string reply;
  std::string err;
  if (!send_command(socket_path, command, &reply, &err)) {
    std::fprintf(stderr, "usbdisplay_evdi_ctl: %s\n", err.c_str());
    return 1;
  }
  std::printf("%s", reply.c_str());
  return reply.rfind("OK", 0) == 0 ? 0 : 1;
}
