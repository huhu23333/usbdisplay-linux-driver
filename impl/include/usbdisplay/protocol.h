// SPDX-License-Identifier: MIT
// IP 网络传输路径的分组与解包定义。USB 模式使用 hid 目录下的 HID 报告封装。
//
// WireFormat::LegacyEzV1 使用 32 字节小端序头部，后接访问单元的分片。
//   [0..3]   魔数 "EZDP"
//   [4]      版本号，取值为 1
//   [5]      类型：1 视频，2 音频，3 配置（JSON）
//   [6]      标志：bit0 关键帧，bit1 访问单元起始，bit2 访问单元结束，
//            bit3 配置，bit4 不连续
//   [7]      流编号：0 视频，1 音频
//   [8..11]  包序号，循环回绕
//   [12..15] 帧编号，循环回绕
//   [16..17] 分片数量
//   [18..19] 分片位置，从零开始
//   [20..23] 访问单元总字节数
//   [24..31] 显示时间戳，单位微秒，无符号
//   [32..]   负载
//
// WireFormat::RtpAvc 使用标准 RTP 承载 H.264，时间戳频率为 90 kHz，
// 支持单个 NAL 单元、STAP-A 与 FU-A 三种打包方式。
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace usbdisplay {
namespace proto {

constexpr size_t kLegacyHeaderSize = 32;
constexpr uint32_t kLegacyMagic = 0x50445A45u;  // 魔数 "EZDP" 的小端序取值
constexpr size_t kTcpLengthPrefixSize = 4;

// 网络传输实验路径的默认端口，可通过设备选择参数覆盖。
// USB 模式不使用这些端口。
constexpr uint16_t kProvisionalMediaTcpPort = 8000;
constexpr uint16_t kProvisionalVideoUdpPort = 8000;
constexpr uint16_t kProvisionalAudioUdpPort = 8002;
constexpr uint16_t kProvisionalControlTcpPort = 8001;

enum class WireFormat : uint8_t {
  LegacyEzV1 = 0,
  RtpAvc = 1,
};

const char* wire_format_name(WireFormat fmt);
bool wire_format_from_name(const std::string& name, WireFormat* out);

enum class MediaKind : uint8_t {
  Video = 1,
  Audio = 2,
  Config = 3,
};

enum PacketFlags : uint8_t {
  kFlagKeyframe = 1u << 0,
  kFlagAuStart = 1u << 1,
  kFlagAuEnd = 1u << 2,
  kFlagConfig = 1u << 3,
  kFlagDiscontinuity = 1u << 4,
};

struct PacketizeOptions {
  WireFormat format = WireFormat::LegacyEzV1;
  // 每个媒体报文的最大负载字节数，UDP 传输时需小于路径 MTU。
  size_t max_payload = 1400;
  uint8_t stream_id = 0;
  uint32_t ssrc = 0x455A4D52u;  // "EZMR"
  uint8_t rtp_payload_type = 96;
  // legacy 格式经 TCP 传输时，为每个报文添加四字节大端序长度前缀；
  // RTP 使用数据报传输，不使用该前缀。
  bool length_prefix = false;
};

// 解析后的媒体报文。负载指针指向输入缓冲区，输入缓冲区释放后失效。
struct ParsedPacket {
  WireFormat format = WireFormat::LegacyEzV1;
  MediaKind kind = MediaKind::Video;
  uint8_t flags = 0;
  uint8_t stream_id = 0;
  uint32_t seq = 0;
  uint32_t frame_id = 0;
  uint16_t split_count = 1;
  uint16_t split_position = 0;
  uint32_t au_size = 0;
  uint64_t pts_us = 0;
  bool keyframe = false;
  const uint8_t* payload = nullptr;
  size_t payload_len = 0;

  // RTP 专用字段。
  uint16_t rtp_seq = 0;
  uint32_t rtp_timestamp = 0;
  uint32_t ssrc = 0;
  bool marker = false;
};

class Packetizer {
 public:
  Packetizer();
  explicit Packetizer(const PacketizeOptions& options);

  void set_options(const PacketizeOptions& options);
  const PacketizeOptions& options() const { return options_; }
  void reset();

  uint32_t next_seq() const { return seq_; }

  // 把一个访问单元或一段音频负载拆分为若干报文并追加到 out。
  // frame_id 按访问单元递增，pts_us 的单位为微秒。
  void packetize(const uint8_t* data, size_t len, MediaKind kind,
                 uint64_t pts_us, bool keyframe, uint32_t frame_id,
                 std::vector<std::vector<uint8_t>>& out);

  // 便捷接口：生成携带 UTF-8 JSON 内容的配置报文。
  void packetize_config(const std::string& json, uint64_t pts_us,
                        std::vector<std::vector<uint8_t>>& out);

  // TCP 长度前缀工具，使用四字节大端序。
  static void append_length_prefix(std::vector<uint8_t>& packet);
  // 数据完整时返回真，consumed 设为本次使用的字节数，包括前缀与负载。
  static bool strip_length_prefix(const uint8_t* data, size_t len,
                                  size_t* consumed, const uint8_t** payload,
                                  size_t* payload_len);

 private:
  PacketizeOptions options_;
  uint32_t seq_ = 0;
  uint32_t rtp_timestamp_base_ = 0;
};

class Depacketizer {
 public:
  explicit Depacketizer(WireFormat format = WireFormat::LegacyEzV1);
  void set_format(WireFormat format) { format_ = format; }

  // 解析一个完整报文，TCP 传输时长度前缀已被去除。
  bool parse(const uint8_t* packet, size_t len, ParsedPacket* out,
             std::string* err) const;

 private:
  WireFormat format_;
};

// Annex-B 访问单元工具，当前仅支持 H.264。
namespace annexb {

struct NalUnit {
  const uint8_t* data = nullptr;  // includes the start code
  size_t size = 0;
  uint8_t type = 0;   // nal_unit_type (data[4] or data[3] & 0x1f)
  bool is_vcl = false;
  bool is_keyframe = false;  // IDR (5) or SPS/PPS ahead of it
};

struct AccessUnit {
  const uint8_t* data = nullptr;
  size_t size = 0;
  uint64_t pts_us = 0;
  bool keyframe = false;
  std::vector<NalUnit> nals;
};

// 把 Annex-B 码流拆分为访问单元。以 AUD 或 first_mb_in_slice 为零处作为边界，
// 把前置的 SPS、PPS、SEI 归入后面的图像。输入格式错误时返回假。
bool split_access_units(const uint8_t* data, size_t len,
                        std::vector<AccessUnit>* out, std::string* err);

// 返回访问单元中出现的 NAL 单元类型，供诊断使用。
std::string describe_access_unit(const AccessUnit& au);

}  // namespace annexb

}  // namespace proto
}  // namespace usbdisplay
