// SPDX-License-Identifier: MIT
// 模拟接收器，用于协议层测试。
//
// 支持校验 USB 视频 URB 转储文件，也支持通过 TCP 与 UDP 接收 IP 通道报文。
// 检查封装、访问单元、序号、显示时间戳与分片字段并输出统计，
// 校验通过时返回零，失败时返回一。
//
//   fake_device --usb-dump /tmp/urbs.bin --expect-frames 6
//   fake_device --tcp-port 9000 --expect-frames 30
//   fake_device --udp-port 9000 --wire rtp
#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>
#include <vector>

#include "common/log.h"
#include "common/util.h"
#include "hid/hid_profile.h"
#include "hid/report_dump.h"
#include "proto/nal_scan.h"
#include "usbdisplay/protocol.h"

namespace {

volatile std::sig_atomic_t g_stop = 0;
void on_signal(int) { g_stop = 1; }

struct Options {
  uint16_t tcp_port = 0;  // 0 = disabled unless explicitly set
  uint16_t udp_port = 0;
  bool tcp_enabled = false;
  bool udp_enabled = false;
  std::string bind_host = "127.0.0.1";
  std::string wire = "legacy-v1";
  std::string report_path;
  int expect_frames = 0;
  double max_seconds = 30.0;
  size_t hex_bytes = 0;
  bool no_length_prefix = false;
  bool quiet = false;
  // HID 报告转储模式，供控制通道与历史格式使用。
  std::string hid_dump_path;
  uint32_t hid_report_size = 0;
  bool header_per_chunk = false;
  // USB 视频模式：校验 4096 字节 URB 转储文件。
  std::string usb_dump_path;
  uint32_t urb_size = 0;
};

struct Verifier {
  usbdisplay::proto::WireFormat format = usbdisplay::proto::WireFormat::LegacyEzV1;
  usbdisplay::proto::Depacketizer depacketizer;

  uint64_t packets = 0;
  uint64_t bytes = 0;
  uint64_t video_aus = 0;
  uint64_t video_keyframes = 0;
  uint64_t audio_packets = 0;
  uint64_t audio_bytes = 0;
  uint64_t seq_gaps = 0;
  uint64_t au_errors = 0;
  uint64_t pts_errors = 0;
  uint64_t parse_errors = 0;
  uint64_t idr_mismatch = 0;
  uint64_t marker_errors = 0;
  uint64_t rtp_au_packets = 0;
  uint64_t hid_control_messages = 0;
  uint64_t aud_missing = 0;
  uint64_t first_packet_bytes = 0;

  bool have_seq[2] = {false, false};
  uint32_t last_seq[2] = {0, 0};
  bool have_pts[2] = {false, false};
  uint64_t last_pts[2] = {0, 0};

  struct AuBuf {
    std::vector<uint8_t> data;
    uint32_t expected_size = 0;
    uint16_t expected_split = 0;
    uint8_t flags = 0;
    bool keyframe = false;
  };
  std::map<uint32_t, AuBuf> au_buffers;    // legacy: frame_id -> AU
  std::map<uint32_t, std::vector<uint8_t>> rtp_aus;  // rtp: timestamp -> AU

  std::vector<uint8_t> first_packet;
  std::vector<uint8_t> first_video_au;

  explicit Verifier(usbdisplay::proto::WireFormat fmt)
      : format(fmt), depacketizer(fmt) {}

  void note_seq(const usbdisplay::proto::ParsedPacket& pkt) {
    const int stream = (pkt.kind == usbdisplay::proto::MediaKind::Audio) ? 1 : 0;
    if (!have_seq[stream]) {
      have_seq[stream] = true;
      last_seq[stream] = pkt.seq;
      return;
    }
    const uint32_t expected = last_seq[stream] + 1;
    if (pkt.seq != expected) {
      ++seq_gaps;
      if (!quiet) {
        std::fprintf(stderr, "seq gap: stream %d expected %u got %u\n", stream,
                     expected, pkt.seq);
      }
    }
    last_seq[stream] = pkt.seq;
  }

  bool quiet = false;

  void note_pts(const usbdisplay::proto::ParsedPacket& pkt) {
    const int stream = (pkt.kind == usbdisplay::proto::MediaKind::Audio) ? 1 : 0;
    if (have_pts[stream] && pkt.pts_us < last_pts[stream]) {
      ++pts_errors;
      if (!quiet) {
        std::fprintf(stderr, "pts regression: stream %d %llu -> %llu\n", stream,
                     static_cast<unsigned long long>(last_pts[stream]),
                     static_cast<unsigned long long>(pkt.pts_us));
      }
    }
    have_pts[stream] = true;
    last_pts[stream] = pkt.pts_us;
  }

  void on_legacy(const usbdisplay::proto::ParsedPacket& pkt) {
    note_seq(pkt);
    note_pts(pkt);
    if (pkt.kind == usbdisplay::proto::MediaKind::Audio) {
      ++audio_packets;
      audio_bytes += pkt.payload_len;
      return;
    }
    if (pkt.kind != usbdisplay::proto::MediaKind::Video) return;  // config
    AuBuf& buf = au_buffers[pkt.frame_id];
    if (pkt.flags & usbdisplay::proto::kFlagAuStart) {
      buf.data.clear();
      buf.expected_size = pkt.au_size;
      buf.expected_split = pkt.split_count;
      buf.keyframe = pkt.keyframe;
    }
    if (buf.expected_size == 0) buf.expected_size = pkt.au_size;
    if (buf.expected_split == 0) buf.expected_split = pkt.split_count;
    buf.flags |= pkt.flags;
    buf.data.insert(buf.data.end(), pkt.payload, pkt.payload + pkt.payload_len);
    if (pkt.flags & usbdisplay::proto::kFlagAuEnd) {
      if (buf.data.size() != pkt.au_size) {
        ++au_errors;
        if (!quiet) {
          std::fprintf(stderr, "AU %u size mismatch: got %zu expected %u\n",
                       pkt.frame_id, buf.data.size(), pkt.au_size);
        }
      }
      // 根据实际 NAL 单元校验关键帧标记。
      bool has_idr = false;
      std::vector<usbdisplay::proto::detail::NalRange> nals;
      if (usbdisplay::proto::detail::scan_nal_units(buf.data.data(),
                                                    buf.data.size(), &nals)) {
        for (const auto& nal : nals) {
          if (nal.type == 5) has_idr = true;
        }
      }
      if (has_idr != buf.keyframe) ++idr_mismatch;
      if (has_idr) ++video_keyframes;
      if (first_video_au.empty()) first_video_au = buf.data;
      ++video_aus;
      au_buffers.erase(pkt.frame_id);
    }
  }

  void on_rtp(const usbdisplay::proto::ParsedPacket& pkt) {
    note_seq(pkt);
    note_pts(pkt);
    std::vector<uint8_t>& au = rtp_aus[pkt.rtp_timestamp];
    if (pkt.flags & usbdisplay::proto::kFlagAuStart) {
      au.clear();
    }
    au.insert(au.end(), pkt.payload, pkt.payload + pkt.payload_len);
    ++rtp_au_packets;
    if (pkt.keyframe) {
      // 本访问单元包含 IDR 图像
    }
    const bool au_end = (pkt.flags & usbdisplay::proto::kFlagAuEnd) != 0;
    if (au_end && pkt.marker) {
      // 标记位只应出现在最后一个报文，下面进行校验。
    }
    if (au_end) {
      if (pkt.keyframe) ++video_keyframes;
      if (first_video_au.empty()) first_video_au = au;
      ++video_aus;
      rtp_aus.erase(pkt.rtp_timestamp);
    } else if (pkt.marker) {
      ++marker_errors;  // marker on a non-final fragment
    }
  }

  bool feed(const uint8_t* data, size_t len) {
    usbdisplay::proto::ParsedPacket pkt;
    std::string err;
    if (!depacketizer.parse(data, len, &pkt, &err)) {
      ++parse_errors;
      if (!quiet) std::fprintf(stderr, "parse error: %s\n", err.c_str());
      return false;
    }
    ++packets;
    bytes += len;
    if (first_packet.empty()) {
      first_packet.assign(data, data + std::min<size_t>(len, 64));
      first_packet_bytes = len;
    }
    if (format == usbdisplay::proto::WireFormat::RtpAvc) {
      on_rtp(pkt);
    } else {
      on_legacy(pkt);
    }
    return true;
  }

  bool passed(const Options& options) const {
    if (parse_errors != 0 || seq_gaps != 0 || au_errors != 0 ||
        pts_errors != 0 || idr_mismatch != 0 || marker_errors != 0 ||
        aud_missing != 0) {
      return false;
    }
    if (options.expect_frames > 0 &&
        video_aus < static_cast<uint64_t>(options.expect_frames)) {
      return false;
    }
    return packets > 0;
  }

  std::string report(const Options& options) const {
    std::string out;
    char line[512];
    std::snprintf(line, sizeof(line), "fake_device report\nwire: %s\n",
                  options.wire.c_str());
    out += line;
    std::snprintf(line, sizeof(line), "bind: %s tcp_port=%u udp_port=%u\n",
                  options.bind_host.c_str(), options.tcp_port,
                  options.udp_port);
    out += line;
    std::snprintf(line, sizeof(line),
                  "packets: %llu bytes: %llu video_aus: %llu keyframes: %llu "
                  "audio_packets: %llu audio_bytes: %llu\n",
                  static_cast<unsigned long long>(packets),
                  static_cast<unsigned long long>(bytes),
                  static_cast<unsigned long long>(video_aus),
                  static_cast<unsigned long long>(video_keyframes),
                  static_cast<unsigned long long>(audio_packets),
                  static_cast<unsigned long long>(audio_bytes));
    out += line;
    std::snprintf(line, sizeof(line),
                  "errors: seq_gaps=%llu au_errors=%llu pts_errors=%llu "
                  "parse_errors=%llu idr_mismatch=%llu marker_errors=%llu\n",
                  static_cast<unsigned long long>(seq_gaps),
                  static_cast<unsigned long long>(au_errors),
                  static_cast<unsigned long long>(pts_errors),
                  static_cast<unsigned long long>(parse_errors),
                  static_cast<unsigned long long>(idr_mismatch),
                  static_cast<unsigned long long>(marker_errors));
    out += line;
    if (options.hex_bytes > 0 && !first_packet.empty()) {
      out += "first_packet:\n";
      out += usbdisplay::hexdump(first_packet.data(), first_packet.size(),
                                 options.hex_bytes);
    }
    if (options.hex_bytes > 0 && !first_video_au.empty()) {
      out += "first_video_au:\n";
      out += usbdisplay::hexdump(first_video_au.data(), first_video_au.size(),
                                 options.hex_bytes);
    }
    out += passed(options) ? "RESULT: PASS\n" : "RESULT: FAIL\n";
    return out;
  }
};

void write_report(const Options& options, const Verifier& verifier) {
  const std::string text = verifier.report(options);
  std::printf("%s", text.c_str());
  std::fflush(stdout);
  if (!options.report_path.empty()) {
    std::string err;
    usbdisplay::write_file(options.report_path, text.data(), text.size(), &err);
    if (!err.empty()) std::fprintf(stderr, "report write failed: %s\n", err.c_str());
  }
}

bool should_stop(const Options& options, const Verifier& verifier,
                 uint64_t start_us) {
  if (g_stop) return true;
  if (options.expect_frames > 0 &&
      verifier.video_aus >= static_cast<uint64_t>(options.expect_frames)) {
    return true;
  }
  if (options.max_seconds > 0 &&
      (usbdisplay::mono_us() - start_us) / 1e6 >= options.max_seconds) {
    return true;
  }
  return false;
}

int make_listen_socket(const std::string& host, uint16_t port, int type,
                       uint16_t* actual_port) {
  const int fd = ::socket(AF_INET, type | SOCK_CLOEXEC, 0);
  if (fd < 0) return -1;
  int one = 1;
  ::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
  struct sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_port = htons(port);
  if (::inet_pton(AF_INET, host.c_str(), &addr.sin_addr) != 1) {
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  }
  if (::bind(fd, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) < 0) {
    std::fprintf(stderr, "bind %s:%u failed: %s\n", host.c_str(), port,
                 std::strerror(errno));
    ::close(fd);
    return -1;
  }
  socklen_t len = sizeof(addr);
  ::getsockname(fd, reinterpret_cast<struct sockaddr*>(&addr), &len);
  if (actual_port) *actual_port = ntohs(addr.sin_port);
  if (type == SOCK_STREAM && ::listen(fd, 4) < 0) {
    std::fprintf(stderr, "listen failed: %s\n", std::strerror(errno));
    ::close(fd);
    return -1;
  }
  return fd;
}

bool process_tcp_stream(Verifier* verifier, std::vector<uint8_t>* buffer,
                        bool length_prefix) {
  for (;;) {
    if (length_prefix) {
      const uint8_t* payload = nullptr;
      size_t payload_len = 0;
      size_t consumed = 0;
      if (!usbdisplay::proto::Packetizer::strip_length_prefix(
              buffer->data(), buffer->size(), &consumed, &payload,
              &payload_len)) {
        return true;  // need more data
      }
      if (!verifier->feed(payload, payload_len)) return false;
      buffer->erase(buffer->begin(),
                    buffer->begin() + static_cast<long>(consumed));
    } else {
      // 无长度前缀的 TCP 流把整个缓冲块视为一个报文。
      if (buffer->empty()) return true;
      if (!verifier->feed(buffer->data(), buffer->size())) return false;
      buffer->clear();
    }
  }
}

void run_tcp(const Options& options, Verifier* verifier, bool* any_received) {
  uint16_t actual_port = 0;
  const int listen_fd = make_listen_socket(options.bind_host, options.tcp_port,
                                           SOCK_STREAM, &actual_port);
  if (listen_fd < 0) return;
  std::printf("LISTENING tcp %s:%u\n", options.bind_host.c_str(), actual_port);
  std::fflush(stdout);
  const uint64_t start_us = usbdisplay::mono_us();
  std::vector<uint8_t> buffer;
  bool client_open = false;
  int client_fd = -1;
  while (!should_stop(options, *verifier, start_us)) {
    if (!client_open) {
      struct pollfd pfd{};
      pfd.fd = listen_fd;
      pfd.events = POLLIN;
      const int rc = ::poll(&pfd, 1, 100);
      if (rc <= 0) continue;
      client_fd = ::accept(listen_fd, nullptr, nullptr);
      if (client_fd < 0) continue;
      client_open = true;
      std::printf("CLIENT connected\n");
      std::fflush(stdout);
      continue;
    }
    struct pollfd pfd{};
    pfd.fd = client_fd;
    pfd.events = POLLIN;
    const int rc = ::poll(&pfd, 1, 50);
    if (rc < 0) break;
    if (rc == 0) continue;
    uint8_t chunk[65536];
    const ssize_t n = ::recv(client_fd, chunk, sizeof(chunk), 0);
    if (n < 0) {
      if (errno == EINTR) continue;
      break;
    }
    if (n == 0) {
      std::printf("CLIENT closed\n");
      break;
    }
    *any_received = true;
    buffer.insert(buffer.end(), chunk, chunk + n);
    if (!process_tcp_stream(verifier, &buffer, !options.no_length_prefix)) {
      break;
    }
  }
  if (client_fd >= 0) ::close(client_fd);
  ::close(listen_fd);
}

void run_udp(const Options& options, Verifier* verifier, bool* any_received) {
  uint16_t actual_port = 0;
  const int fd = make_listen_socket(options.bind_host, options.udp_port,
                                    SOCK_DGRAM, &actual_port);
  if (fd < 0) return;
  std::printf("LISTENING udp %s:%u\n", options.bind_host.c_str(), actual_port);
  std::fflush(stdout);
  const uint64_t start_us = usbdisplay::mono_us();
  while (!should_stop(options, *verifier, start_us)) {
    struct pollfd pfd{};
    pfd.fd = fd;
    pfd.events = POLLIN;
    const int rc = ::poll(&pfd, 1, 100);
    if (rc <= 0) continue;
    uint8_t datagram[65536];
    const ssize_t n = ::recv(fd, datagram, sizeof(datagram), 0);
    if (n <= 0) continue;
    *any_received = true;
    verifier->feed(datagram, static_cast<size_t>(n));
  }
  ::close(fd);
}

struct HidStreamState {
  std::vector<uint8_t> pending;
  uint32_t expected = 0;
  bool in_message = false;
};

void process_hid_message(Verifier* verifier, const std::vector<uint8_t>& message,
                         bool* have_seq, uint32_t* last_seq) {
  if (message.empty()) return;
  if (message[0] == '{' || message[0] == '[') {
    ++verifier->hid_control_messages;
    if (!verifier->quiet) {
      std::printf("HID control message: %s\n",
                  std::string(reinterpret_cast<const char*>(message.data()),
                              std::min<size_t>(message.size(), 256))
                      .c_str());
    }
    return;
  }
  const bool raw_annexb =
      (message.size() >= 4 && message[0] == 0 && message[1] == 0 &&
       (message[2] == 1 || (message[2] == 0 && message.size() >= 4 &&
                            message[3] == 1)));
  if (raw_annexb) {
    bool has_idr = false;
    std::vector<usbdisplay::proto::detail::NalRange> nals;
    if (usbdisplay::proto::detail::scan_nal_units(message.data(),
                                                  message.size(), &nals)) {
      for (const auto& nal : nals) {
        if (nal.type == 5) has_idr = true;
      }
    } else {
      ++verifier->au_errors;
    }
    if (has_idr) ++verifier->video_keyframes;
    if (verifier->first_video_au.empty()) verifier->first_video_au = message;
    ++verifier->video_aus;
    verifier->bytes += message.size();
    return;
  }
  if (message.size() < usbdisplay::hid::kInnerHeaderSize) {
    ++verifier->au_errors;
    return;
  }
  const bool ugreen = (message.size() >= 4 &&
                       (std::memcmp(message.data(), "RRIM", 4) == 0 ||
                        std::memcmp(message.data(), "TADV", 4) == 0));
  uint32_t seq = 0;
  uint32_t declared_length = 0;
  if (ugreen) {
    declared_length = usbdisplay::read_le32(message.data() + 4);
    seq = usbdisplay::read_le32(message.data() + 12);
    if (declared_length != message.size() - 8) ++verifier->au_errors;
  } else {
    usbdisplay::hid::InnerHeader header;
    std::string err;
    if (!usbdisplay::hid::parse_inner_header(message.data(), message.size(),
                                              &header, &err)) {
      ++verifier->au_errors;
      return;
    }
    if (header.magic != usbdisplay::hid::kInnerMagic) ++verifier->au_errors;
    declared_length = header.length;
    seq = header.seq;
    if (header.length != message.size() - 8) ++verifier->au_errors;
    if (header.one != 1) ++verifier->au_errors;
  }
  if (*have_seq) {
    if (seq != *last_seq + 1) ++verifier->seq_gaps;
  }
  *have_seq = true;
  *last_seq = seq;

  const uint8_t* payload = message.data() + usbdisplay::hid::kInnerHeaderSize;
  const size_t payload_len = message.size() - usbdisplay::hid::kInnerHeaderSize;
  bool has_idr = false;
  std::vector<usbdisplay::proto::detail::NalRange> nals;
  if (usbdisplay::proto::detail::scan_nal_units(payload, payload_len, &nals)) {
    for (const auto& nal : nals) {
      if (nal.type == 5) has_idr = true;
    }
  }
  if (has_idr) ++verifier->video_keyframes;
  if (verifier->first_video_au.empty()) {
    verifier->first_video_au.assign(payload, payload + payload_len);
  }
  ++verifier->video_aus;
  verifier->bytes += payload_len;
  (void)declared_length;
}

int run_hid_dump(const Options& options, Verifier* verifier) {
  std::vector<usbdisplay::hid::ReportDumpRecord> records;
  uint32_t report_size = 0;
  std::string err;
  if (!usbdisplay::hid::read_report_dump(options.hid_dump_path, &records,
                                         &report_size, &err)) {
    std::fprintf(stderr, "hid dump: %s\n", err.c_str());
    return 2;
  }
  if (options.hid_report_size != 0 && report_size != options.hid_report_size) {
    std::fprintf(stderr, "hid dump: report size %u != expected %u\n",
                 report_size, options.hid_report_size);
    ++verifier->au_errors;
  }
  std::map<uint8_t, HidStreamState> streams;
  bool have_seq = false;
  uint32_t last_seq = 0;
  for (const auto& record : records) {
    if (record.report.size() < usbdisplay::hid::kOuterHeaderSize ||
        record.report.size() != report_size) {
      ++verifier->au_errors;
      continue;
    }
    const uint8_t report_id = record.report[0];
    usbdisplay::hid::OuterHeader outer;
    std::string herr;
    if (!usbdisplay::hid::parse_outer_header(record.report.data(),
                                             record.report.size(), &outer,
                                             &herr)) {
      ++verifier->au_errors;
      continue;
    }
    // 每个报告都写入总负载长度，负载从偏移 16 开始，不超过报告长度减 16。
    size_t payload_len = outer.length;
    if (payload_len > record.report.size() - usbdisplay::hid::kOuterHeaderSize) {
      payload_len = record.report.size() - usbdisplay::hid::kOuterHeaderSize;
    }
    HidStreamState& state = streams[report_id];
    if (!state.in_message) {
      if (outer.length == 0 ||
          outer.length > 64u * 1024 * 1024) {
        ++verifier->au_errors;
        continue;
      }
      state.expected = outer.length;
      state.in_message = true;
    }
    const size_t want = state.expected - state.pending.size();
    const size_t take = std::min(want, payload_len);
    state.pending.insert(
        state.pending.end(),
        record.report.begin() + usbdisplay::hid::kOuterHeaderSize,
        record.report.begin() + usbdisplay::hid::kOuterHeaderSize + take);
    ++verifier->packets;
    if (state.pending.size() >= state.expected) {
      process_hid_message(verifier, state.pending, &have_seq, &last_seq);
      state.pending.clear();
      state.expected = 0;
      state.in_message = false;
    }
  }
  if (verifier->first_packet.empty() && !records.empty()) {
    verifier->first_packet = records[0].report;
  }
  if (options.expect_frames > 0 &&
      verifier->video_aus < static_cast<uint64_t>(options.expect_frames)) {
    std::fprintf(stderr, "hid dump: only %llu/%d video frames\n",
                 static_cast<unsigned long long>(verifier->video_aus),
                 options.expect_frames);
  }
  std::printf("HID dump: %zu reports, report_size=%u, video_frames=%llu, "
              "control_messages=%llu\n",
              records.size(), report_size,
              static_cast<unsigned long long>(verifier->video_aus),
              static_cast<unsigned long long>(verifier->hid_control_messages));
  return 0;
}

int run_usb_dump(const Options& options, Verifier* verifier) {
  std::vector<usbdisplay::hid::ReportDumpRecord> records;
  uint32_t urb_size = 0;
  std::string err;
  if (!usbdisplay::hid::read_report_dump(options.usb_dump_path, &records,
                                         &urb_size, &err)) {
    std::fprintf(stderr, "usb dump: %s\n", err.c_str());
    return 2;
  }
  if (options.urb_size != 0 && urb_size != options.urb_size) {
    std::fprintf(stderr, "usb dump: URB size %u != expected %u\n", urb_size,
                 options.urb_size);
    ++verifier->au_errors;
  }
  if (urb_size <= 16) {
    std::fprintf(stderr, "usb dump: invalid URB size %u\n", urb_size);
    return 2;
  }
  struct State {
    std::vector<uint8_t> data;
    uint32_t expected = 0;
    bool active = false;
  } state;
  for (const auto& record : records) {
    const std::vector<uint8_t>& urb = record.report;
    if (urb.size() != urb_size) {
      ++verifier->au_errors;
      continue;
    }
    if (urb[0] != 0x02) {
      std::fprintf(stderr, "usb dump: unexpected report id 0x%02x\n", urb[0]);
      ++verifier->au_errors;
      continue;
    }
    if (!state.active) {
      const uint32_t length = usbdisplay::read_be32(urb.data() + 1);
      const uint32_t width = usbdisplay::read_be32(urb.data() + 8);
      const uint32_t height = usbdisplay::read_be32(urb.data() + 12);
      if (length == 0 || width == 0 || height == 0) {
        std::fprintf(stderr,
                     "usb dump: bad first URB header L=%u %ux%u\n", length,
                     width, height);
        ++verifier->au_errors;
        continue;
      }
      if (urb[5] != 0 || urb[6] != 0 || urb[7] != 0) {
        std::fprintf(stderr, "usb dump: first URB bytes 5..7 not zero\n");
        ++verifier->au_errors;
      }
      state.active = true;
      state.expected = length;
      state.data.clear();
    } else {
      for (int i = 1; i < 16; ++i) {
        if (urb[i] != 0) {
          std::fprintf(stderr, "usb dump: continuation header not zero\n");
          ++verifier->au_errors;
          break;
        }
      }
    }
    const size_t remaining = state.expected - state.data.size();
    const size_t take = std::min<size_t>(remaining, urb_size - 16);
    state.data.insert(state.data.end(), urb.begin() + 16,
                      urb.begin() + 16 + take);
    ++verifier->packets;
    if (state.data.size() < state.expected) continue;

    const uint8_t* frame = state.data.data();
    const size_t frame_len = state.data.size();
    const bool start_code = frame_len >= 3 && frame[0] == 0 && frame[1] == 0 &&
                            (frame[2] == 1 ||
                             (frame_len >= 4 && frame[3] == 1));
    if (!start_code) {
      ++verifier->au_errors;
      std::fprintf(stderr, "usb dump: frame lost its Annex-B start code\n");
    }
    std::vector<usbdisplay::proto::detail::NalRange> nals;
    bool has_idr = false;
    bool first_is_aud = false;
    if (usbdisplay::proto::detail::scan_nal_units(frame, frame_len, &nals)) {
      for (size_t i = 0; i < nals.size(); ++i) {
        if (nals[i].type == 5) has_idr = true;
      }
      if (!nals.empty() && nals[0].type == 9) first_is_aud = true;
    } else {
      ++verifier->au_errors;
    }
    if (!first_is_aud) ++verifier->aud_missing;
    if (has_idr) ++verifier->video_keyframes;
    if (verifier->first_video_au.empty()) {
      verifier->first_video_au = state.data;
    }
    ++verifier->video_aus;
    verifier->bytes += frame_len;
    state.active = false;
  }
  if (state.active) ++verifier->au_errors;
  if (verifier->first_packet.empty() && !records.empty()) {
    verifier->first_packet = records[0].report;
  }
  if (options.expect_frames > 0 &&
      verifier->video_aus < static_cast<uint64_t>(options.expect_frames)) {
    std::fprintf(stderr, "usb dump: only %llu/%d video frames\n",
                 static_cast<unsigned long long>(verifier->video_aus),
                 options.expect_frames);
  }
  std::printf(
      "USB dump: %zu URBs, urb_size=%u, video_frames=%llu, aud_missing=%llu\n",
      records.size(), urb_size,
      static_cast<unsigned long long>(verifier->video_aus),
      static_cast<unsigned long long>(verifier->aud_missing));
  return 0;
}

void print_usage() {
  std::printf(
      "usage: fake_device [--tcp-port N] [--udp-port N] [options]\n"
      "  --tcp-port N         listen for length-prefixed TCP media packets\n"
      "  --udp-port N         listen for UDP media datagrams\n"
      "  --bind HOST          bind address (default 127.0.0.1)\n"
      "  --wire legacy-v1|rtp wire format to parse (default legacy-v1)\n"
      "  --expect-frames N    exit after N validated video access units\n"
      "  --max-seconds SEC    exit after SEC seconds (default 30)\n"
      "  --report FILE        write the summary report to FILE\n"
      "  --hex-bytes N        dump the first N bytes of the first packet/AU\n"
      "  --no-length-prefix   treat TCP data as raw packets (no 4-byte size)\n"
      "  --usb-dump FILE      validate a USBDRPT1 dump of 4096-B EP2 video URBs\n"
      "  --urb-size N         expected URB size (default: from dump)\n"
      "  --hid-dump FILE      validate a USBDRPT1 report dump instead of sockets\n"
      "  --hid-report-size N  expected HID report size (default: from dump)\n"
      "  --header-per-chunk   continuation reports repeat the 16-byte header\n"
      "  --quiet              suppress per-error diagnostics\n");
}

}  // namespace

int main(int argc, char** argv) {
  Options options;
  bool have_tcp = false;
  bool have_udp = false;
  for (int i = 1; i < argc; ++i) {
    const std::string arg = argv[i];
    auto next = [&](std::string* out) -> bool {
      if (i + 1 >= argc) return false;
      *out = argv[++i];
      return true;
    };
    if (arg == "--tcp-port") {
      std::string v;
      if (!next(&v)) return 2;
      options.tcp_port = static_cast<uint16_t>(std::strtoul(v.c_str(), nullptr, 0));
      have_tcp = true;
    } else if (arg == "--udp-port") {
      std::string v;
      if (!next(&v)) return 2;
      options.udp_port = static_cast<uint16_t>(std::strtoul(v.c_str(), nullptr, 0));
      have_udp = true;
    } else if (arg == "--bind") {
      if (!next(&options.bind_host)) return 2;
    } else if (arg == "--wire") {
      if (!next(&options.wire)) return 2;
    } else if (arg == "--report") {
      if (!next(&options.report_path)) return 2;
    } else if (arg == "--expect-frames") {
      std::string v;
      if (!next(&v)) return 2;
      options.expect_frames = std::atoi(v.c_str());
    } else if (arg == "--max-seconds") {
      std::string v;
      if (!next(&v)) return 2;
      options.max_seconds = std::atof(v.c_str());
    } else if (arg == "--hex-bytes") {
      std::string v;
      if (!next(&v)) return 2;
      options.hex_bytes = static_cast<size_t>(std::strtoul(v.c_str(), nullptr, 0));
    } else if (arg == "--usb-dump") {
      if (!next(&options.usb_dump_path)) return 2;
    } else if (arg == "--urb-size") {
      std::string v;
      if (!next(&v)) return 2;
      options.urb_size =
          static_cast<uint32_t>(std::strtoul(v.c_str(), nullptr, 0));
    } else if (arg == "--hid-dump") {
      if (!next(&options.hid_dump_path)) return 2;
    } else if (arg == "--hid-report-size") {
      std::string v;
      if (!next(&v)) return 2;
      options.hid_report_size =
          static_cast<uint32_t>(std::strtoul(v.c_str(), nullptr, 0));
    } else if (arg == "--header-per-chunk") {
      options.header_per_chunk = true;
    } else if (arg == "--no-length-prefix") {
      options.no_length_prefix = true;
    } else if (arg == "--quiet") {
      options.quiet = true;
    } else if (arg == "--help" || arg == "-h") {
      print_usage();
      return 0;
    } else {
      std::fprintf(stderr, "unknown argument: %s\n", arg.c_str());
      print_usage();
      return 2;
    }
  }
  if (!options.usb_dump_path.empty()) {
    Verifier usb_verifier(usbdisplay::proto::WireFormat::LegacyEzV1);
    usb_verifier.quiet = options.quiet;
    const int rc = run_usb_dump(options, &usb_verifier);
    if (rc != 0) return rc;
    Options report_options = options;
    report_options.wire = "usb-urb";
    write_report(report_options, usb_verifier);
    return usb_verifier.passed(options) ? 0 : 1;
  }
  if (!options.hid_dump_path.empty()) {
    Verifier hid_verifier(usbdisplay::proto::WireFormat::LegacyEzV1);
    hid_verifier.quiet = options.quiet;
    const int rc = run_hid_dump(options, &hid_verifier);
    if (rc != 0) return rc;
    write_report(options, hid_verifier);
    return hid_verifier.passed(options) ? 0 : 1;
  }
  if (!have_tcp && !have_udp) {
    std::fprintf(stderr,
                 "need --tcp-port/--udp-port or --hid-dump FILE\n");
    return 2;
  }
  std::signal(SIGINT, on_signal);
  std::signal(SIGTERM, on_signal);
  usbdisplay::log::set_level(usbdisplay::TraceLevel::Warn);

  usbdisplay::proto::WireFormat format = usbdisplay::proto::WireFormat::LegacyEzV1;
  if (!usbdisplay::proto::wire_format_from_name(options.wire, &format)) {
    std::fprintf(stderr, "unknown wire format: %s\n", options.wire.c_str());
    return 2;
  }
  Verifier verifier(format);
  verifier.quiet = options.quiet;
  bool any_received = false;

  // 同一时间只运行一种监听方式。
  if (have_tcp && have_udp) {
    std::fprintf(stderr,
                 "please use either --tcp-port or --udp-port, not both\n");
    return 2;
  }
  if (have_tcp) {
    if (format == usbdisplay::proto::WireFormat::RtpAvc) {
      std::fprintf(stderr, "rtp wire format requires --udp-port\n");
      return 2;
    }
    run_tcp(options, &verifier, &any_received);
  } else {
    run_udp(options, &verifier, &any_received);
  }
  write_report(options, verifier);
  return verifier.passed(options) ? 0 : 1;
}
