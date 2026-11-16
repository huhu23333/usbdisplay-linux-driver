// SPDX-License-Identifier: MIT
#include "transport/hidraw.h"

#include <dirent.h>
#include <fcntl.h>
#include <linux/hidraw.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include <cerrno>
#include <chrono>
#include <climits>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>
#include <thread>

#include "common/log.h"
#include "common/util.h"

namespace usbdisplay {
namespace transport {
namespace {

std::string read_text(const std::string& path) {
  std::ifstream f(path);
  if (!f) return "";
  std::ostringstream ss;
  ss << f.rdbuf();
  return ss.str();
}

// 把 HID_ID 属性解析为总线编号、厂商编号与产品编号。
void parse_hid_id(const std::string& value, uint16_t* bustype, uint16_t* vid,
                  uint16_t* pid) {
  unsigned bus = 0, vendor = 0, product = 0;
  if (std::sscanf(value.c_str(), "%x:%x:%x", &bus, &vendor, &product) == 3) {
    *bustype = static_cast<uint16_t>(bus);
    *vid = static_cast<uint16_t>(vendor);
    *pid = static_cast<uint16_t>(product);
  }
}

std::string dirname(const std::string& path) {
  const auto pos = path.find_last_of('/');
  return pos == std::string::npos ? "" : path.substr(0, pos);
}

// 解析 hidraw 节点对应的 USB 接口目录并读取接口编号。
int interface_number_for(const std::string& sysfs_hidraw) {
  char resolved[PATH_MAX];
  const std::string devdir_link = sysfs_hidraw + "/device";
  if (::realpath(devdir_link.c_str(), resolved) == nullptr) return -1;
  const std::string devdir(resolved);
  const std::string ifdir = dirname(devdir);
  if (ifdir.empty()) return -1;
  const std::string num = trim(read_text(ifdir + "/bInterfaceNumber"));
  if (num.empty()) return -1;
  return static_cast<int>(std::strtol(num.c_str(), nullptr, 16));
}

}  // namespace

std::vector<HidrawInfo> enumerate_hidraw(std::string* err) {
  std::vector<HidrawInfo> out;
  DIR* dir = ::opendir("/sys/class/hidraw");
  if (dir == nullptr) {
    if (err) *err = std::string("opendir /sys/class/hidraw: ") + std::strerror(errno);
    return out;
  }
  while (struct dirent* ent = ::readdir(dir)) {
    const std::string name = ent->d_name;
    if (name == "." || name == "..") continue;
    const std::string sysfs = "/sys/class/hidraw/" + name;
    const std::string uevent = read_text(sysfs + "/device/uevent");
    if (uevent.empty()) continue;
    HidrawInfo info;
    info.sysfs = sysfs;
    info.path = "/dev/" + name;
    std::istringstream lines(uevent);
    std::string line;
    while (std::getline(lines, line)) {
      const auto eq = line.find('=');
      if (eq == std::string::npos) continue;
      const std::string key = line.substr(0, eq);
      const std::string value = trim(line.substr(eq + 1));
      if (key == "HID_ID") {
        parse_hid_id(value, &info.bustype, &info.vid, &info.pid);
      } else if (key == "HID_NAME") {
        info.name = value;
      } else if (key == "HID_PHYS") {
        info.phys = value;
      } else if (key == "HID_UNIQ") {
        info.uniq = value;
      }
    }
    info.interface_number = interface_number_for(sysfs);
    info.readable = ::access(info.path.c_str(), R_OK) == 0;
    info.writable = ::access(info.path.c_str(), W_OK) == 0;
    out.push_back(std::move(info));
  }
  ::closedir(dir);
  return out;
}

bool find_hidraw_by_interface(uint16_t vid, uint16_t pid, int interface_number,
                              HidrawInfo* out, std::string* err) {
  const auto list = enumerate_hidraw(err);
  for (const auto& info : list) {
    if (info.vid == vid && info.pid == pid &&
        info.interface_number == interface_number) {
      *out = info;
      return true;
    }
  }
  if (err) {
    char buf[96];
    std::snprintf(buf, sizeof(buf),
                  "no hidraw node for VID_%04X&PID_%04X interface %d", vid, pid,
                  interface_number);
    *err = buf;
  }
  return false;
}

bool parse_hid_report_descriptor(const std::vector<uint8_t>& descriptor,
                                 HidReportSizes* out, std::string* err) {
  if (out == nullptr) {
    if (err) *err = "null report-size output";
    return false;
  }
  out->input.clear();
  out->output.clear();
  out->feature.clear();
  if (descriptor.empty()) {
    if (err) *err = "empty HID report descriptor";
    return false;
  }
  uint32_t report_size_bits = 0;
  uint32_t report_count = 0;
  uint8_t report_id = 0;
  size_t pos = 0;
  auto read_value = [&](size_t n) -> uint32_t {
    uint32_t v = 0;
    for (size_t i = 0; i < n && pos < descriptor.size(); ++i) {
      v |= static_cast<uint32_t>(descriptor[pos++]) << (8 * i);
    }
    return v;
  };
  while (pos < descriptor.size()) {
    const uint8_t prefix = descriptor[pos++];
    if (prefix == 0xFE) {  // long item
      if (pos + 2 > descriptor.size()) break;
      const uint8_t data_size = descriptor[pos + 1];
      pos += 2;
      pos += data_size;
      continue;
    }
    const uint8_t size_code = prefix & 0x03;
    const uint8_t type = (prefix >> 2) & 0x03;
    const uint8_t tag = (prefix >> 4) & 0x0F;
    const size_t item_size = size_code == 3 ? 4 : size_code;
    if (pos + item_size > descriptor.size()) break;
    const uint32_t value = read_value(item_size);
    if (type == 0) {  // main
      const size_t bytes =
          (static_cast<size_t>(report_size_bits) * report_count + 7) / 8;
      if (tag == 0x8) {  // input
        if (bytes > out->input[report_id]) out->input[report_id] = bytes;
      } else if (tag == 0x9) {  // output
        if (bytes > out->output[report_id]) out->output[report_id] = bytes;
      } else if (tag == 0xB) {  // feature
        if (bytes > out->feature[report_id]) out->feature[report_id] = bytes;
      }
    } else if (type == 1) {  // global
      if (tag == 0x7) {  // report size
        report_size_bits = value;
      } else if (tag == 0x8) {  // report id
        report_id = static_cast<uint8_t>(value);
      } else if (tag == 0x9) {  // report count
        report_count = value;
      }
    }
  }
  if (out->input.empty() && out->output.empty()) {
    if (err) *err = "no input/output reports in HID descriptor";
    return false;
  }
  return true;
}

bool wait_for_hidraw_interface(uint16_t vid, uint16_t pid, int interface_number,
                               int timeout_ms, HidrawInfo* out,
                               std::string* err) {
  const uint64_t deadline = mono_ms() + static_cast<uint64_t>(timeout_ms);
  for (;;) {
    HidrawInfo info;
    if (find_hidraw_by_interface(vid, pid, interface_number, &info, nullptr) &&
        info.readable && info.writable) {
      if (out) *out = info;
      return true;
    }
    if (mono_ms() >= deadline) break;
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
  }
  if (err) {
    char buf[160];
    std::snprintf(buf, sizeof(buf),
                  "hidraw node for VID_%04X&PID_%04X interface %d did not "
                  "appear within %d ms",
                  vid, pid, interface_number, timeout_ms);
    *err = buf;
  }
  return false;
}

bool find_hidraw(uint16_t vid, uint16_t pid, HidrawInfo* out,
                 std::string* err) {
  const auto list = enumerate_hidraw(err);
  for (const auto& info : list) {
    if (info.vid == vid && info.pid == pid) {
      *out = info;
      return true;
    }
  }
  if (err) {
    std::ostringstream ss;
    ss << "no /dev/hidraw matching " << std::hex << std::uppercase
       << "VID_" << vid << "&PID_" << pid << " (found " << std::dec
       << list.size() << " HID devices)";
    *err = ss.str();
  }
  return false;
}

HidrawDevice::~HidrawDevice() { close(); }

bool HidrawDevice::open(const std::string& path, std::string* err) {
  close();
  fd_ = ::open(path.c_str(), O_RDWR | O_CLOEXEC | O_NONBLOCK);
  if (fd_ < 0) {
    if (err) {
      *err = "open " + path + ": " + std::strerror(errno) +
             " (install the one-time udev rule with "
             "sudo tools/install-video-rules.sh)";
    }
    return false;
  }
  path_ = path;
  return true;
}

void HidrawDevice::close() {
  if (fd_ >= 0) {
    ::close(fd_);
    fd_ = -1;
  }
  path_.clear();
}

bool HidrawDevice::get_info(HidrawInfo* out, std::string* err) const {
  if (fd_ < 0) {
    if (err) *err = "hidraw: not open";
    return false;
  }
  *out = HidrawInfo();
  out->path = path_;
  struct hidraw_devinfo info{};
  if (::ioctl(fd_, HIDIOCGRAWINFO, &info) == 0) {
    out->bustype = info.bustype;
    out->vid = static_cast<uint16_t>(info.vendor);
    out->pid = static_cast<uint16_t>(info.product);
  }
  char buf[256];
  const int n = ::ioctl(fd_, HIDIOCGRAWNAME(sizeof(buf)), buf);
  if (n > 0) out->name = std::string(buf, static_cast<size_t>(n));
  const int pn = ::ioctl(fd_, HIDIOCGRAWPHYS(sizeof(buf)), buf);
  if (pn > 0) out->phys = std::string(buf, static_cast<size_t>(pn));
  for (const auto& e : enumerate_hidraw(nullptr)) {
    if (e.path == path_) {
      out->interface_number = e.interface_number;
      break;
    }
  }
  return true;
}

std::vector<uint8_t> HidrawDevice::report_descriptor(std::string* err) const {
  std::vector<uint8_t> desc;
  if (fd_ < 0) {
    if (err) *err = "hidraw: not open";
    return desc;
  }
  int size = 0;
  if (::ioctl(fd_, HIDIOCGRDESCSIZE, &size) < 0 || size <= 0 ||
      size > HID_MAX_DESCRIPTOR_SIZE) {
    if (err) *err = "hidraw: HIDIOCGRDESCSIZE failed";
    return desc;
  }
  struct hidraw_report_descriptor raw{};
  raw.size = static_cast<uint32_t>(size);
  if (::ioctl(fd_, HIDIOCGRDESC, &raw) < 0) {
    if (err) *err = "hidraw: HIDIOCGRDESC failed";
    return desc;
  }
  desc.assign(raw.value, raw.value + raw.size);
  return desc;
}

bool HidrawDevice::write_output_report(uint8_t report_id, const uint8_t* payload,
                                       size_t len, size_t report_size,
                                       std::string* err) {
  if (fd_ < 0) {
    if (err) *err = "hidraw: not open";
    return false;
  }
  if (len > report_size) {
    if (err) {
      *err = "hidraw: payload " + std::to_string(len) +
             " bytes exceeds report size " + std::to_string(report_size);
    }
    return false;
  }
  std::vector<uint8_t> buf(report_size + 1, 0);
  buf[0] = report_id;
  std::memcpy(buf.data() + 1, payload, len);
  const ssize_t n = ::write(fd_, buf.data(), buf.size());
  if (n < 0) {
    if (err) *err = std::string("hidraw: write failed: ") + std::strerror(errno);
    return false;
  }
  return true;
}

bool HidrawDevice::write_feature_report(uint8_t report_id,
                                        const uint8_t* payload, size_t len,
                                        size_t report_size, std::string* err) {
  if (fd_ < 0) {
    if (err) *err = "hidraw: not open";
    return false;
  }
  if (len > report_size) {
    if (err) *err = "hidraw: feature payload too large";
    return false;
  }
  std::vector<uint8_t> buf(report_size + 1, 0);
  buf[0] = report_id;
  std::memcpy(buf.data() + 1, payload, len);
  if (::ioctl(fd_, HIDIOCSFEATURE(buf.size()), buf.data()) < 0) {
    if (err) {
      *err = std::string("hidraw: HIDIOCSFEATURE failed: ") + std::strerror(errno);
    }
    return false;
  }
  return true;
}

bool HidrawDevice::write_raw(const uint8_t* buffer, size_t len,
                             std::string* err) {
  if (fd_ < 0) {
    if (err) *err = "hidraw: not open";
    return false;
  }
  const ssize_t n = ::write(fd_, buffer, len);
  if (n < 0) {
    if (err) {
      *err = std::string("hidraw: raw write failed: ") + std::strerror(errno);
    }
    return false;
  }
  if (static_cast<size_t>(n) != len) {
    if (err) {
      *err = "hidraw: short raw write " + std::to_string(n) + "/" +
             std::to_string(len);
    }
    return false;
  }
  return true;
}

int HidrawDevice::read_report(uint8_t* buf, size_t len, int timeout_ms,
                              std::string* err) {
  if (fd_ < 0) {
    if (err) *err = "hidraw: not open";
    return -1;
  }
  struct pollfd pfd{};
  pfd.fd = fd_;
  pfd.events = POLLIN;
  const int rc = ::poll(&pfd, 1, timeout_ms);
  if (rc == 0) return 0;
  if (rc < 0) {
    if (err) *err = std::string("hidraw: poll failed: ") + std::strerror(errno);
    return -1;
  }
  const ssize_t n = ::read(fd_, buf, len);
  if (n < 0) {
    if (err) *err = std::string("hidraw: read failed: ") + std::strerror(errno);
    return -1;
  }
  return static_cast<int>(n);
}

}  // namespace transport
}  // namespace usbdisplay
