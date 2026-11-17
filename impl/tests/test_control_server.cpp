// SPDX-License-Identifier: MIT
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#include "evdi/control_server.h"
#include "test_harness.h"

namespace {
std::string path_for(const char* name) {
  return "/tmp/usbdisplay-control-" + std::to_string(::getpid()) + "-" + name;
}
class ControlClient {
 public:
  bool connect(const std::string& path) {
    fd_ = ::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd_ < 0) return false;
    sockaddr_un address{};
    address.sun_family = AF_UNIX;
    std::memcpy(address.sun_path, path.c_str(), path.size() + 1);
    if (::connect(fd_, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0) {
      ::close(fd_);
      fd_ = -1;
      return false;
    }
    return true;
  }
  bool command(const std::string& command, std::string* reply) {
    const std::string line = command + "\n";
    if (::send(fd_, line.data(), line.size(), MSG_NOSIGNAL) !=
        static_cast<ssize_t>(line.size())) return false;
    char buffer[256];
    const ssize_t got = ::recv(fd_, buffer, sizeof(buffer), 0);
    if (got <= 0) return false;
    reply->assign(buffer, static_cast<size_t>(got));
    return true;
  }
  void close() { if (fd_ >= 0) { ::close(fd_); fd_ = -1; } }
  ~ControlClient() { close(); }
 private:
  int fd_ = -1;
};
class Poller {
 public:
  explicit Poller(usbdisplay::evdi::ControlServer* server) : server_(server) {
    thread_ = std::thread([this] {
      while (!stop_.load()) {
        server_->poll();
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
      }
    });
  }
  ~Poller() { stop_.store(true); if (thread_.joinable()) thread_.join(); }
 private:
  usbdisplay::evdi::ControlServer* server_;
  std::atomic<bool> stop_{false};
  std::thread thread_;
};
}  // namespace

int main() {
  using namespace usbdisplay::evdi;
  bool connected = false;
  bool fail_connect = false;
  int connect_calls = 0;
  int disconnect_calls = 0;
  ControlServer server(
      [&](int, std::string* err) {
        if (connected) return true;
        ++connect_calls;
        if (fail_connect) { *err = "fake connect failed"; return false; }
        connected = true;
        return true;
      },
      [&]() {
        if (!connected) return;
        ++disconnect_calls;
        connected = false;
      },
      [&]() {
        return std::string("OK connected=") + (connected ? "1" : "0") +
               " width=1920 height=1080 modes=60,120";
      });
  const std::string path = path_for("control");
  std::string err;
  CHECK(server.listen(path, &err));
  Poller poller(&server);
  {
    ControlClient client;
    CHECK(client.connect(path));
    std::string reply;
    CHECK(client.command("STATUS", &reply));
    CHECK(reply.find("connected=0") != std::string::npos);
    CHECK(client.command("CONNECT", &reply));
    CHECK(reply.find("OK connected=1") != std::string::npos);
    CHECK(connected);
    CHECK(client.command("CONNECT", &reply));  // 幂等
    CHECK(reply.find("OK connected=1") != std::string::npos);
    CHECK(connect_calls == 1);
    CHECK(client.command("STATUS", &reply));
    CHECK(reply.find("connected=1") != std::string::npos);
    CHECK(reply.find("modes=60,120") != std::string::npos);
    CHECK(client.command("CONNECT 120", &reply));
    CHECK(reply.find("OK connected=1") != std::string::npos);
    CHECK(connect_calls == 1);
    CHECK(disconnect_calls == 0);
    CHECK(client.command("STATUS", &reply));
    CHECK(reply.find("modes=60,120") != std::string::npos);
    CHECK(client.command("CONNECT 120", &reply));
    CHECK(disconnect_calls == 0);
    CHECK(client.command("CONNECT 42", &reply));
    CHECK(reply.find("unsupported-fps") != std::string::npos);
    CHECK(client.command("DISCONNECT", &reply));
    CHECK(reply.find("OK connected=0") != std::string::npos);
    CHECK(client.command("DISCONNECT", &reply));
    CHECK(reply.find("OK connected=0") != std::string::npos);
    CHECK(disconnect_calls == 1);
    CHECK(client.command("BOGUS", &reply));
    CHECK(reply.find("ERR unknown") != std::string::npos);
  }
  {
    fail_connect = true;
    ControlClient client;
    CHECK(client.connect(path));
    std::string reply;
    CHECK(client.command("CONNECT", &reply));
    CHECK(reply.find("ERR") == 0);
    CHECK(!connected);
    fail_connect = false;
  }
  {
    // 多 client：一个断开不影响状态。
    ControlClient first;
    ControlClient second;
    CHECK(first.connect(path));
    CHECK(second.connect(path));
    std::string reply;
    CHECK(first.command("CONNECT", &reply));
    CHECK(second.command("STATUS", &reply));
    CHECK(reply.find("connected=1") != std::string::npos);
    first.close();
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    CHECK(second.command("STATUS", &reply));
    CHECK(reply.find("connected=1") != std::string::npos);
    CHECK(second.command("DISCONNECT", &reply));
    CHECK(disconnect_calls == 2);
  }
  server.close();
  ::unlink(path.c_str());
  return testharness::finish("test_control_server");
}
