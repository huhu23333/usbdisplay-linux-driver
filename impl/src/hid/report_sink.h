// SPDX-License-Identifier: MIT
// HID 报告通道实现：真实 hidraw、离线转储、空通道。
// 每条逻辑消息按 hid_profile.h 的格式拆分为若干固定长度 HID 报告。
#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace usbdisplay {
namespace hid {

// 外层报告封装格式。App16 用于控制通道，Ugreen14 为早期网络传输格式，当前代码不使用。
enum class OuterFraming : uint8_t { App16 = 0, Ugreen14 = 1 };

class ReportSink {
 public:
  virtual ~ReportSink() = default;
  virtual bool open(std::string* err) = 0;
  virtual void close() = 0;
  virtual bool connected() const = 0;
  // 负载为整条逻辑消息，例如 JSON 文本。
  virtual bool send_message(const uint8_t* payload, size_t len,
                            uint32_t param3, uint32_t param4,
                            std::string* err) = 0;
  // 接收一条逻辑消息，超时未收到数据时 timed_out 置为真。
  virtual bool recv_message(std::string* payload, int timeout_ms,
                            bool* timed_out, std::string* err) = 0;
  virtual std::string describe() const = 0;
  virtual uint8_t report_id() const = 0;
  virtual size_t report_size() const = 0;
  virtual size_t payload_budget() const = 0;
  virtual size_t reports_sent() const = 0;
  virtual size_t messages_sent() const = 0;

  // 选择后续消息使用的外层封装格式。
  void set_ugreen_outer(uint8_t type, uint16_t flags) {
    ugreen_outer_ = true;
    ugreen_type_ = type;
    ugreen_flags_ = flags;
  }
  // App16 格式下是否在每个报告中写入参数三与参数四，默认只写入首个报告。
  void set_params_every_report(bool every) { params_every_report_ = every; }

 protected:
  OuterFraming outer_framing() const {
    return ugreen_outer_ ? OuterFraming::Ugreen14 : OuterFraming::App16;
  }
  bool ugreen_outer_ = false;
  uint8_t ugreen_type_ = 0;
  uint16_t ugreen_flags_ = 0;
  bool params_every_report_ = false;
};

std::unique_ptr<ReportSink> make_hidraw_report_sink(const std::string& path,
                                                    uint8_t report_id,
                                                    size_t report_size,
                                                    bool header_per_chunk,
                                                    size_t chunk_budget = 0);
std::unique_ptr<ReportSink> make_recording_report_sink(const std::string& path,
                                                       uint8_t report_id,
                                                       size_t report_size,
                                                       bool header_per_chunk,
                                                       size_t chunk_budget = 0);
std::unique_ptr<ReportSink> make_null_report_sink(uint8_t report_id,
                                                  size_t report_size,
                                                  size_t chunk_budget = 0);

// 供测试与工具使用：把一条消息拆分为固定长度 HID 报告。
// App16 格式下每个报告都包含报告编号、大端序总负载长度、参数三、参数四和三个零字节，
// 偏移 16 起为负载分片；参数三与参数四默认只写入首个报告。
// Ugreen14 格式下参数三与参数四分别表示序号与分片信息。
bool build_reports(const uint8_t* payload, size_t len, uint8_t report_id,
                   size_t report_size, bool header_per_chunk,
                   uint32_t param3, uint32_t param4,
                   std::vector<std::vector<uint8_t>>* out, std::string* err,
                   size_t chunk_budget = 0,
                   OuterFraming framing = OuterFraming::App16,
                   uint8_t ugreen_type = 0, uint16_t ugreen_flags = 0,
                   bool params_every_report = false);

}  // namespace hid
}  // namespace usbdisplay
