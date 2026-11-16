// SPDX-License-Identifier: MIT
#include <mutex>
#include <vector>

#include "common/log.h"
#include "proto/wire.h"
#include "transport/transport.h"
#include "usbdisplay/protocol.h"

namespace usbdisplay {
namespace transport {
namespace {

class NullSink : public MediaSink {
 public:
  bool open(std::string*) override {
    open_ = true;
    return true;
  }
  bool send(const uint8_t* data, size_t len, std::string*) override {
    bytes_ += len;
    (void)data;
    return true;
  }
  void close() override { open_ = false; }
  bool connected() const override { return open_; }
  std::string describe() const override {
    return "null://" + std::to_string(bytes_) + "B";
  }
  bool datagram() const override { return true; }

 private:
  bool open_ = false;
  uint64_t bytes_ = 0;
};

// 按相同格式解析每个发出的报文，无需硬件或网络即可发现封装错误。
class LoopbackSink : public MediaSink {
 public:
  explicit LoopbackSink(proto::WireFormat format) : depacketizer_(format) {}

  bool open(std::string*) override {
    open_ = true;
    return true;
  }
  bool send(const uint8_t* data, size_t len, std::string* err) override {
    std::lock_guard<std::mutex> lock(mutex_);
    proto::ParsedPacket pkt;
    std::string perr;
    if (!depacketizer_.parse(data, len, &pkt, &perr)) {
      ++parse_errors_;
      if (err) *err = "loopback: " + perr;
      return false;
    }
    ++packets_;
    bytes_ += len;
    if (pkt.kind == proto::MediaKind::Video) {
      if (pkt.flags & proto::kFlagAuStart) ++video_aus_;
      if (pkt.keyframe) ++keyframes_;
    }
    return true;
  }
  void close() override {
    std::lock_guard<std::mutex> lock(mutex_);
    open_ = false;
    log::info("loopback: packets=" + std::to_string(packets_) +
              " bytes=" + std::to_string(bytes_) +
              " video_aus=" + std::to_string(video_aus_) +
              " keyframes=" + std::to_string(keyframes_) +
              " parse_errors=" + std::to_string(parse_errors_));
  }
  bool connected() const override { return open_; }
  std::string describe() const override { return "loopback://"; }
  bool datagram() const override { return true; }

 private:
  proto::Depacketizer depacketizer_;
  std::mutex mutex_;
  bool open_ = false;
  uint64_t packets_ = 0;
  uint64_t bytes_ = 0;
  uint64_t video_aus_ = 0;
  uint64_t keyframes_ = 0;
  uint64_t parse_errors_ = 0;
};

}  // namespace

std::unique_ptr<MediaSink> make_loopback_sink(proto::WireFormat format) {
  return std::unique_ptr<MediaSink>(new LoopbackSink(format));
}
std::unique_ptr<MediaSink> make_null_sink() {
  return std::unique_ptr<MediaSink>(new NullSink());
}

}  // namespace transport
}  // namespace usbdisplay
