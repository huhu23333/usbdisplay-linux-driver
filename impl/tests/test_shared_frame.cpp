// SPDX-License-Identifier: MIT
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <string>
#include <thread>
#include <vector>

#include "evdi/frame_lease_server.h"
#include "evdi/shared_frame.h"
#include "test_harness.h"

namespace {
std::string path_for(const char* suffix) {
  return "/tmp/usbdisplay-shared-" + std::to_string(::getpid()) + "-" + suffix;
}
std::vector<uint8_t> pattern(uint64_t seq, size_t size) {
  std::vector<uint8_t> data(size);
  for (size_t i = 0; i < size; ++i) data[i] = static_cast<uint8_t>((i * 31 + seq) & 0xff);
  return data;
}
class ServerThread {
 public:
  explicit ServerThread(usbdisplay::evdi::FrameLeaseServer* server) : server_(server) {
    thread_ = std::thread([this] {
      while (!stop_.load()) {
        server_->poll();
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
      }
    });
  }
  ~ServerThread() { stop_.store(true); if (thread_.joinable()) thread_.join(); }
 private:
  usbdisplay::evdi::FrameLeaseServer* server_;
  std::atomic<bool> stop_{false};
  std::thread thread_;
};
}  // namespace

int main() {
  using namespace usbdisplay::evdi;
  std::string err;
  {
    SharedFrameLayout layout;
    CHECK(validate_shared_layout(layout, &err));
    layout.width = 1280;
    CHECK(!validate_shared_layout(layout, &err));
  }
  {
    const std::string shm = path_for("basic");
    const std::string sock = path_for("basic.sock");
    SharedFrameLayout layout;
    SharedFrameWriter writer(layout);
    CHECK(writer.open(shm, &err));
    struct stat info {};
    CHECK(stat(shm.c_str(), &info) == 0 && (info.st_mode & 0777) == 0660);
    FrameLeaseServer server(&writer);
    CHECK(server.listen(sock, &err));
    ServerThread thread(&server);
    const auto frame = pattern(1, layout.frame_bytes());
    CHECK(writer.publish(frame.data(), static_cast<int>(layout.stride), &err));
    SharedFrameReader reader;
    CHECK(reader.open(shm, sock, &err));
    SharedFrameView view;
    CHECK(reader.read(&view, &err) == FrameReadResult::Ok);
    CHECK(view.data == frame && view.seq == 1);
    CHECK(reader.read(&view, &err) == FrameReadResult::NoFrame);
    reader.close();
    server.close();
    writer.close();
    ::unlink(shm.c_str());
  }
  {
    // 旧 reader 的 inode 被新 writer 替换后必须报 Error，新 reader 可重开读取。
    const std::string shm = path_for("replace");
    const std::string sock = path_for("replace.sock");
    SharedFrameLayout layout;
    SharedFrameWriter writer1(layout);
    CHECK(writer1.open(shm, &err));
    FrameLeaseServer server1(&writer1);
    CHECK(server1.listen(sock, &err));
    {
      ServerThread thread1(&server1);
      const auto frame = pattern(5, layout.frame_bytes());
      CHECK(writer1.publish(frame.data(), static_cast<int>(layout.stride), &err));
      SharedFrameReader old_reader;
      CHECK(old_reader.open(shm, sock, &err));
      SharedFrameView old_view;
      CHECK(old_reader.read(&old_view, &err) == FrameReadResult::Ok);
      server1.close();
      writer1.close();
      SharedFrameWriter writer2(layout);
      CHECK(writer2.open(shm, &err));
      FrameLeaseServer server2(&writer2);
      CHECK(server2.listen(sock, &err));
      ServerThread thread2(&server2);
      const auto frame2 = pattern(6, layout.frame_bytes());
      CHECK(writer2.publish(frame2.data(), static_cast<int>(layout.stride), &err));
      SharedFrameView stale_view;
      CHECK(old_reader.read(&stale_view, &err) == FrameReadResult::Error);
      old_reader.close();
      SharedFrameReader new_reader;
      CHECK(new_reader.open(shm, sock, &err));
      SharedFrameView new_view;
      CHECK(new_reader.read(&new_view, &err) == FrameReadResult::Ok);
      CHECK(new_view.data == frame2);
      new_reader.close();
      server2.close();
      writer2.close();
    }
    ::unlink(shm.c_str());
  }
  {
    const std::string shm = path_for("active");
    SharedFrameLayout layout;
    SharedFrameWriter writer1(layout);
    CHECK(writer1.open(shm, &err));
    SharedFrameWriter writer2(layout);
    CHECK(!writer2.open(shm, &err));
    writer1.close();
  }
  {
    const std::string shm = path_for("inode");
    const std::string replacement = path_for("replacement");
    SharedFrameLayout layout;
    SharedFrameWriter writer(layout);
    CHECK(writer.open(shm, &err));
    const int fd = ::open(replacement.c_str(), O_CREAT | O_WRONLY | O_TRUNC, 0600);
    CHECK(fd >= 0 && ::write(fd, "x", 1) == 1);
    ::close(fd);
    CHECK(::rename(replacement.c_str(), shm.c_str()) == 0);
    writer.close();
    CHECK(::access(shm.c_str(), F_OK) == 0);
    ::unlink(shm.c_str());
  }
  {
    const std::string shm = path_for("fork");
    const std::string sock = path_for("fork.sock");
    SharedFrameLayout layout;
    SharedFrameWriter writer(layout);
    CHECK(writer.open(shm, &err));
    FrameLeaseServer server(&writer);
    CHECK(server.listen(sock, &err));
    ServerThread thread(&server);
    const auto frame = pattern(2, layout.frame_bytes());
    CHECK(writer.publish(frame.data(), static_cast<int>(layout.stride), &err));
    pid_t pid = ::fork();
    CHECK(pid >= 0);
    if (pid == 0) {
      SharedFrameReader reader;
      std::string child_err;
      if (!reader.open(shm, sock, &child_err)) _exit(10);
      SharedFrameView view;
      if (reader.read(&view, &child_err) != FrameReadResult::Ok) _exit(11);
      if (view.data != frame) _exit(12);
      _exit(0);
    }
    int status = 0;
    CHECK(::waitpid(pid, &status, 0) == pid);
    CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 0);
    server.close();
    writer.close();
    ::unlink(shm.c_str());
  }
  return testharness::finish("test_shared_frame");
}
