// SPDX-License-Identifier: MIT
// 控制通道抽象接口，当前实现为 HID 上的 JSON-RPC。
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

namespace usbdisplay {
namespace control {

class ControlChannel {
 public:
  virtual ~ControlChannel() = default;
  virtual bool open(std::string* err) = 0;
  virtual void close() = 0;
  virtual bool connected() const = 0;
  // 发送一条 JSON 文本消息，报文封装由具体通道实现。
  virtual bool send_json(const std::string& json, std::string* err) = 0;
  // 接收一条 JSON 文本消息，超时或出错时返回假，
  // 超时且没有数据时把 timed_out 置为真。
  virtual bool recv_json(std::string* json, int timeout_ms, bool* timed_out,
                         std::string* err) = 0;
  virtual std::string describe() const = 0;
};

}  // namespace control
}  // namespace usbdisplay
