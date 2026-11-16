// SPDX-License-Identifier: MIT
// 媒体传输抽象接口。打包器生成完整报文，发送端只负责搬运字节或数据报。
#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>

#include "usbdisplay/protocol.h"
#include "usbdisplay/types.h"

namespace usbdisplay {
namespace transport {

class MediaSink {
 public:
  virtual ~MediaSink() = default;
  virtual bool open(std::string* err) = 0;
  virtual bool send(const uint8_t* data, size_t len, std::string* err) = 0;
  virtual void close() = 0;
  virtual bool connected() const = 0;
  virtual std::string describe() const = 0;
  // 每次发送为数据报时返回真，字节流传输返回假。
  virtual bool datagram() const = 0;
};

std::unique_ptr<MediaSink> make_tcp_sink(const std::string& host, uint16_t port,
                                         const std::string& bind_address,
                                         int connect_timeout_ms);
std::unique_ptr<MediaSink> make_udp_sink(const std::string& host, uint16_t port,
                                         const std::string& bind_address);
// 进程内校验通道：按相同格式解析收到的报文，供离线演练与测试使用。
std::unique_ptr<MediaSink> make_loopback_sink(proto::WireFormat format);
std::unique_ptr<MediaSink> make_null_sink();

}  // namespace transport
}  // namespace usbdisplay
