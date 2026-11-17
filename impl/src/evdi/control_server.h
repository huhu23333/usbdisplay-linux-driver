// SPDX-License-Identifier: MIT
// evdi 虚拟显示开放控制 socket：CONNECT/DISCONNECT/STATUS。
#pragma once
#include <cstdint>
#include <functional>
#include <string>
#include <vector>
namespace usbdisplay { namespace evdi {
class ControlServer {
 public:
  using ConnectFn = std::function<bool(int fps, std::string*)>;
  using DisconnectFn = std::function<void()>;
  using StatusFn = std::function<std::string()>;
  ControlServer(ConnectFn connect, DisconnectFn disconnect, StatusFn status);
  ~ControlServer();
  ControlServer(const ControlServer&) = delete;
  ControlServer& operator=(const ControlServer&) = delete;
  bool listen(const std::string& path, std::string* err);
  void poll();
  void close();
  bool listening() const;
 private:
  struct Client {
    int fd = -1;
    std::string input;
    std::string output;
    uint64_t last_activity_ms = 0;
  };
  void process(Client* client);
  void reply(Client* client, const std::string& line);
  void close_client(size_t index);
  ConnectFn connect_;
  DisconnectFn disconnect_;
  StatusFn status_;
  std::string path_;
  int listen_fd_ = -1;
  std::vector<Client> clients_;
};
}}  // namespace
