// SPDX-License-Identifier: MIT
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#include "evdi/frame_lease_client.h"
#include "evdi/frame_lease_protocol.h"
#include "evdi/frame_lease_server.h"
#include "evdi/shared_frame.h"
#include "test_harness.h"

namespace {
std::string path_for(const char* name) {
  return "/tmp/usbdisplay-lease-" + std::to_string(::getpid()) + "-" + name;
}
std::vector<uint8_t> pattern(uint64_t seed, size_t size) {
  std::vector<uint8_t> data(size);
  for (size_t i = 0; i < size; ++i) data[i] = static_cast<uint8_t>((i + seed) & 0xff);
  return data;
}
class Poller {
 public:
  explicit Poller(usbdisplay::evdi::FrameLeaseServer* server) : server_(server) {
    thread_ = std::thread([this] {
      while (!stop_.load()) {
        server_->poll();
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
      }
    });
  }
  ~Poller() { stop_.store(true); if (thread_.joinable()) thread_.join(); }
 private:
  usbdisplay::evdi::FrameLeaseServer* server_;
  std::atomic<bool> stop_{false};
  std::thread thread_;
};
}  // namespace

int main() {
  using namespace usbdisplay::evdi;
  std::string err;
  SharedFrameLayout layout;
  const std::string shm = path_for("frames");
  const std::string sock = path_for("frames.sock");
  SharedFrameWriter writer(layout);
  CHECK(writer.open(shm, &err));
  FrameLeaseServer server(&writer);
  CHECK(server.listen(sock, &err));
  Poller poller(&server);
  {
    FrameLeaseClient client;
    CHECK(client.connect(sock, &err));
    LeaseReply reply;
    CHECK(client.acquire(&reply, &err) == LeaseResult::Empty);  // 无帧
    client.close();
  }
  const auto frame = pattern(1, layout.frame_bytes());
  CHECK(writer.publish(frame.data(), static_cast<int>(layout.stride), &err));
  {
    FrameLeaseClient client;
    CHECK(client.connect(sock, &err));
    LeaseReply reply;
    CHECK(client.acquire(&reply, &err) == LeaseResult::Ok);
    CHECK(reply.status == LeaseStatus::Ok);
    CHECK(reply.seq == 1);
    CHECK(reply.offset + reply.payload <= layout.total_size());
    CHECK(client.release(reply.token, &err));
    CHECK(!client.release(reply.token, &err));  // 双 release 被拒绝
    client.close();
  }
  {
    // 过期/伪造 token 不影响当前租约。
    CHECK(writer.publish(frame.data(), static_cast<int>(layout.stride), &err));
    FrameLeaseClient client;
    CHECK(client.connect(sock, &err));
    LeaseReply reply;
    CHECK(client.acquire(&reply, &err) == LeaseResult::Ok);
    CHECK(!client.release(reply.token ^ 1U, &err));
    CHECK(!client.connected());
    client.close();
    CHECK(writer.publish(frame.data(), static_cast<int>(layout.stride), &err));
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    FrameLeaseClient replacement;
    CHECK(replacement.connect(sock, &err));
    LeaseReply replacement_reply;
    CHECK(replacement.acquire(&replacement_reply, &err) == LeaseResult::Ok);
    CHECK(replacement.release(replacement_reply.token, &err));
    replacement.close();
  }
  {
    // client 断开时 server 自动释放租约。
    CHECK(writer.publish(frame.data(), static_cast<int>(layout.stride), &err));
    FrameLeaseClient client;
    CHECK(client.connect(sock, &err));
    LeaseReply reply;
    CHECK(client.acquire(&reply, &err) == LeaseResult::Ok);
    client.close();
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    CHECK(writer.publish(frame.data(), static_cast<int>(layout.stride), &err));
    FrameLeaseClient second;
    CHECK(second.connect(sock, &err));
    LeaseReply second_reply;
    CHECK(second.acquire(&second_reply, &err) == LeaseResult::Ok);
    CHECK(second.release(second_reply.token, &err));
    second.close();
  }
  {
    // fork 子进程崩溃持租约后，server 释放，新 client 可取值。
    CHECK(writer.publish(frame.data(), static_cast<int>(layout.stride), &err));
    const pid_t pid = ::fork();
    CHECK(pid >= 0);
    if (pid == 0) {
      FrameLeaseClient client;
      std::string child_err;
      if (!client.connect(sock, &child_err)) _exit(20);
      LeaseReply reply;
      if (client.acquire(&reply, &child_err) != LeaseResult::Ok) _exit(21);
      _exit(0);  // 不 release
    }
    int status = 0;
    CHECK(::waitpid(pid, &status, 0) == pid);
    CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 0);
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    CHECK(writer.publish(frame.data(), static_cast<int>(layout.stride), &err));
    FrameLeaseClient client;
    CHECK(client.connect(sock, &err));
    LeaseReply reply;
    CHECK(client.acquire(&reply, &err) == LeaseResult::Ok);
    CHECK(client.release(reply.token, &err));
    client.close();
  }
  {
    // 空闲连接与半包连接会被 server 超时关闭，不阻塞新 client。
    server.set_timeouts(30, 30, 100);
    FrameLeaseClient idle;
    CHECK(idle.connect(sock, &err));
    std::this_thread::sleep_for(std::chrono::milliseconds(80));
    CHECK(writer.publish(frame.data(), static_cast<int>(layout.stride), &err));
    FrameLeaseClient after_idle;
    CHECK(after_idle.connect(sock, &err));
    LeaseReply after_idle_reply;
    CHECK(after_idle.acquire(&after_idle_reply, &err) == LeaseResult::Ok);
    CHECK(after_idle.release(after_idle_reply.token, &err));
    after_idle.close();
    idle.close();

    const int raw = ::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    CHECK(raw >= 0);
    sockaddr_un address{};
    address.sun_family = AF_UNIX;
    std::memcpy(address.sun_path, sock.c_str(), sock.size() + 1);
    CHECK(::connect(raw, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0);
    const uint8_t half[4] = {1, 1, 12, 0};
    CHECK(::send(raw, half, sizeof(half), 0) == static_cast<ssize_t>(sizeof(half)));
    std::this_thread::sleep_for(std::chrono::milliseconds(80));
    CHECK(writer.publish(frame.data(), static_cast<int>(layout.stride), &err));
    FrameLeaseClient after_half;
    CHECK(after_half.connect(sock, &err));
    LeaseReply after_half_reply;
    CHECK(after_half.acquire(&after_half_reply, &err) == LeaseResult::Ok);
    CHECK(after_half.release(after_half_reply.token, &err));
    after_half.close();
    ::close(raw);
  }
  {
    // 非阻塞短写：client 不读回复时 server 缓存尾部，不阻塞也不静默关闭。
    server.set_timeouts(1000, 1000, 1000);
    const int raw = ::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    CHECK(raw >= 0);
    const int receive_buffer = 1024;
    setsockopt(raw, SOL_SOCKET, SO_RCVBUF, &receive_buffer,
               sizeof(receive_buffer));
    sockaddr_un address{};
    address.sun_family = AF_UNIX;
    std::memcpy(address.sun_path, sock.c_str(), sock.size() + 1);
    CHECK(::connect(raw, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0);
    LeaseRequest request;
    request.command = LeaseCommand::Acquire;
    uint8_t encoded[kLeaseRequestSize];
    encode_lease_request(request, encoded);
    std::vector<uint8_t> burst;
    for (int i = 0; i < 5000; ++i) {
      burst.insert(burst.end(), encoded, encoded + sizeof(encoded));
    }
    CHECK(::send(raw, burst.data(), burst.size(), 0) > 0);
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    CHECK(server.pending_output() > 0);
    ::close(raw);
  }
  server.close();
  writer.close();
  ::unlink(shm.c_str());
  return testharness::finish("test_frame_lease");
}
