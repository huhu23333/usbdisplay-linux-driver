// SPDX-License-Identifier: MIT
// Media transport abstraction. The packetizer emits complete wire packets;
// sinks only move bytes/datagrams.
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
  // True when each send() is a datagram (UDP); false for byte streams (TCP).
  virtual bool datagram() const = 0;
};

std::unique_ptr<MediaSink> make_tcp_sink(const std::string& host, uint16_t port,
                                         const std::string& bind_address,
                                         int connect_timeout_ms);
std::unique_ptr<MediaSink> make_udp_sink(const std::string& host, uint16_t port,
                                         const std::string& bind_address);
// In-process validating sink (parses what it receives with the same wire
// format); used for --dry-run and tests.
std::unique_ptr<MediaSink> make_loopback_sink(proto::WireFormat format);
std::unique_ptr<MediaSink> make_null_sink();

}  // namespace transport
}  // namespace usbdisplay
