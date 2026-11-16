// SPDX-License-Identifier: MIT
// USB Display 控制通道使用的 hidraw 访问层，不依赖 hidapi。
#pragma once

#include <cstddef>
#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace usbdisplay {
namespace transport {

struct HidrawInfo {
  std::string path;             // /dev/hidrawN
  std::string sysfs;            // /sys/class/hidraw/hidrawN
  std::string name;             // HID_NAME
  std::string phys;             // HID_PHYS
  std::string uniq;             // HID_UNIQ
  uint16_t bustype = 0;
  uint16_t vid = 0;
  uint16_t pid = 0;
  int interface_number = -1;    // USB bInterfaceNumber, -1 if unknown
  bool readable = false;
  bool writable = false;
};

std::vector<HidrawInfo> enumerate_hidraw(std::string* err);
bool find_hidraw(uint16_t vid, uint16_t pid, HidrawInfo* out, std::string* err);
// 按 USB 接口编号查找对应的 hidraw 节点。
bool find_hidraw_by_interface(uint16_t vid, uint16_t pid, int interface_number,
                              HidrawInfo* out, std::string* err);

// 轮询等待指定 USB 接口出现可读写的 hidraw 节点，供设备复位后使用。
bool wait_for_hidraw_interface(uint16_t vid, uint16_t pid, int interface_number,
                               int timeout_ms, HidrawInfo* out,
                               std::string* err);

// 从 HID 报告描述符解析出的各报告编号数据长度，不含报告编号字节。
struct HidReportSizes {
  std::map<uint8_t, size_t> input;
  std::map<uint8_t, size_t> output;
  std::map<uint8_t, size_t> feature;
};
bool parse_hid_report_descriptor(const std::vector<uint8_t>& descriptor,
                                 HidReportSizes* out, std::string* err);

class HidrawDevice {
 public:
  HidrawDevice() = default;
  ~HidrawDevice();
  HidrawDevice(const HidrawDevice&) = delete;
  HidrawDevice& operator=(const HidrawDevice&) = delete;

  bool open(const std::string& path, std::string* err);
  void close();
  bool is_open() const { return fd_ >= 0; }
  int fd() const { return fd_; }
  const std::string& path() const { return path_; }

  bool get_info(HidrawInfo* out, std::string* err) const;
  std::vector<uint8_t> report_descriptor(std::string* err) const;

  // 写入一条 HID 输出报告，负载不含报告编号字节，缓冲区补齐到指定长度。
  bool write_output_report(uint8_t report_id, const uint8_t* payload,
                           size_t len, size_t report_size, std::string* err);
  // 通过 HIDIOCSFEATURE 写入特性报告。
  bool write_feature_report(uint8_t report_id, const uint8_t* payload,
                            size_t len, size_t report_size, std::string* err);
  // 读取一条输入报告，返回字节数、零表示超时、负值表示错误。
  int read_report(uint8_t* buf, size_t len, int timeout_ms, std::string* err);

  // 原样写入一条 HID 报告，首字节为报告编号，长度必须与内核要求一致。
  bool write_raw(const uint8_t* buffer, size_t len, std::string* err);

 private:
  int fd_ = -1;
  std::string path_;
};

}  // namespace transport
}  // namespace usbdisplay
