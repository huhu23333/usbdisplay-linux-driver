// SPDX-License-Identifier: MIT
// 诊断工具：枚举接收器、HID 控制通道与网络接口，或运行离线自检。
//
//   usbdisplay_diag --offline     不访问硬件，自检编解码路径
//   usbdisplay_diag --probe-json  发送心跳并打印设备回复
//   usbdisplay_diag               完整枚举与诊断
//
// 返回零表示诊断完成，一表示离线自检失败，二表示参数错误。
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "common/json.h"
#include "common/log.h"
#include "common/util.h"
#include "proto/nal_scan.h"
#include "audio/pulse_card.h"
#include "audio/pulse_discovery.h"
#include "hid/hid_profile.h"
#include "hid/report_sink.h"
#include "proto/wav.h"
#include "transport/hidraw.h"
#include "transport/libusb_probe.h"
#include "transport/netinfo.h"
#include "usbdisplay/protocol.h"
#include "usbdisplay/version.h"

namespace {

int g_failures = 0;
int g_checks = 0;

void check(bool condition, const std::string& what) {
  ++g_checks;
  if (condition) {
    std::printf("  [ok]   %s\n", what.c_str());
  } else {
    ++g_failures;
    std::printf("  [FAIL] %s\n", what.c_str());
  }
}

void make_synthetic_annexb(std::vector<uint8_t>* out, int frames) {
  out->clear();
  auto append_nal = [out](uint8_t header, size_t payload, uint8_t fill) {
    out->push_back(0);
    out->push_back(0);
    out->push_back(0);
    out->push_back(1);
    out->push_back(header);
    for (size_t i = 0; i < payload; ++i) out->push_back(fill);
  };
  // 依次生成 SPS、PPS、IDR 与非关键帧图像。
  append_nal(0x67, 32, 0x11);  // SPS
  append_nal(0x68, 8, 0x22);   // PPS
  for (int f = 0; f < frames; ++f) {
    if (f > 0) {
      out->push_back(0);
      out->push_back(0);
      out->push_back(1);
      out->push_back(0x41);  // non-IDR slice, nal_ref_idc=2, type=1
      out->push_back(0x88);  // first_mb_in_slice ue(v)==0 marker (MSB set)
      for (size_t i = 0; i < 64 + static_cast<size_t>(f) * 7; ++i) {
        out->push_back(0xAB);
      }
    } else {
      out->push_back(0);
      out->push_back(0);
      out->push_back(1);
      out->push_back(0x65);  // IDR slice type=5
      out->push_back(0x88);
      for (size_t i = 0; i < 512; ++i) out->push_back(0xCD);
    }
  }
}

std::vector<uint8_t> make_wav_pcm16(uint32_t rate, uint16_t channels,
                                    size_t frames) {
  const uint32_t data_size =
      static_cast<uint32_t>(frames * channels * 2);
  std::vector<uint8_t> wav(44 + data_size, 0);
  std::memcpy(wav.data(), "RIFF", 4);
  usbdisplay::write_le32(wav.data() + 4, 36 + data_size);
  std::memcpy(wav.data() + 8, "WAVE", 4);
  std::memcpy(wav.data() + 12, "fmt ", 4);
  usbdisplay::write_le32(wav.data() + 16, 16);
  usbdisplay::write_le16(wav.data() + 20, 1);  // PCM
  usbdisplay::write_le16(wav.data() + 22, channels);
  usbdisplay::write_le32(wav.data() + 24, rate);
  usbdisplay::write_le32(wav.data() + 28, rate * channels * 2);
  usbdisplay::write_le16(wav.data() + 32, static_cast<uint16_t>(channels * 2));
  usbdisplay::write_le16(wav.data() + 34, 16);
  std::memcpy(wav.data() + 36, "data", 4);
  usbdisplay::write_le32(wav.data() + 40, data_size);
  for (size_t i = 0; i < frames * channels; ++i) {
    const int16_t sample = static_cast<int16_t>((i * 37) % 2000 - 1000);
    usbdisplay::write_le16(wav.data() + 44 + i * 2,
                           static_cast<uint16_t>(sample));
  }
  return wav;
}

int run_offline_selftest() {
  using namespace usbdisplay;
  std::printf("usbdisplay_diag offline self-test (%s)\n", version_string());

  // 版本与 JSON 解析。
  check(std::string(version_string()).find("usbdisplay") == 0, "version string");
  {
    const std::string text =
        "{\"jsonrpc\":\"2.0\",\"method\":\"heartbeat\",\"params\":"
        "{\"session-id\":\"abc\\\"def\"},\"id\":7}";
    json::Value value;
    std::string err;
    check(json::Value::parse(text, &value, &err), "JSON parse (" + err + ")");
    check(value.find("method") != nullptr &&
              value["method"].as_string() == "heartbeat",
          "JSON field method");
    check(value["params"]["session-id"].as_string() == "abc\"def",
          "JSON escaped string round-trip");
    json::Value reparsed;
    check(json::Value::parse(value.dump(), &reparsed, &err),
          "JSON re-parse of dump()");
    check(reparsed.dump() == value.dump(), "JSON dump stable");
    json::Value bad;
    check(!json::Value::parse("{\"a\":}", &bad, &err), "JSON rejects bad input");
  }

  // 合成 Annex-B 码流与访问单元拆分。
  std::vector<uint8_t> stream;
  make_synthetic_annexb(&stream, 4);
  std::vector<proto::annexb::AccessUnit> aus;
  std::string err;
  check(proto::annexb::split_access_units(stream.data(), stream.size(), &aus,
                                          &err),
        "Annex-B AU split (" + err + ")");
  check(aus.size() == 4, "Annex-B AU count == 4 (got " +
                             std::to_string(aus.size()) + ")");
  if (aus.size() == 4) {
    check(aus[0].keyframe, "AU0 is keyframe");
    check(aus[0].nals.size() == 3, "AU0 has SPS+PPS+IDR");
    check(!aus[1].keyframe, "AU1 is not keyframe");
    check(aus[0].data == stream.data(), "AU0 starts at buffer start");
    check(aus[3].data + aus[3].size == stream.data() + stream.size(),
          "last AU ends at buffer end");
  }

  // legacy 打包与解包往返。
  {
    proto::PacketizeOptions options;
    options.format = proto::WireFormat::LegacyEzV1;
    options.max_payload = 1400;
    proto::Packetizer packetizer(options);
    std::vector<std::vector<uint8_t>> packets;
    std::vector<uint8_t> payload(3000);
    for (size_t i = 0; i < payload.size(); ++i) {
      payload[i] = static_cast<uint8_t>(i * 31 + 7);
    }
    packetizer.packetize(payload.data(), payload.size(),
                         proto::MediaKind::Video, 1234567, true, 42, packets);
    check(packets.size() == 3, "legacy 3000B -> 3 packets (got " +
                                   std::to_string(packets.size()) + ")");
    proto::Depacketizer depacketizer;
    uint64_t received = 0;
    uint32_t expected_seq = 0;
    bool all_ok = true;
    for (size_t i = 0; i < packets.size(); ++i) {
      proto::ParsedPacket parsed;
      std::string perr;
      if (!depacketizer.parse(packets[i].data(), packets[i].size(), &parsed,
                              &perr)) {
        all_ok = false;
        break;
      }
      all_ok &= parsed.seq == expected_seq++;
      all_ok &= parsed.split_count == 3;
      all_ok &= parsed.split_position == i;
      all_ok &= parsed.pts_us == 1234567;
      all_ok &= parsed.frame_id == 42;
      all_ok &= parsed.keyframe;
      all_ok &= parsed.au_size == payload.size();
      received += parsed.payload_len;
    }
    check(all_ok, "legacy header fields + seq");
    check(received == payload.size(), "legacy reassembled byte count");
    // 固定头部字节。
    const uint8_t* h = packets[0].data();
    check(usbdisplay::read_le32(h) == 0x50445A45u, "legacy magic EZDP");
    check(h[4] == 1 && h[5] == 1 && h[6] == 3, "legacy version/kind/flags");
    check(usbdisplay::read_le64(h + 24) == 1234567, "legacy pts offset 24");
  }

  // TCP 长度前缀。
  {
    proto::PacketizeOptions options;
    options.length_prefix = true;
    options.max_payload = 100;
    proto::Packetizer packetizer(options);
    std::vector<std::vector<uint8_t>> packets;
    std::vector<uint8_t> payload(250, 0x5A);
    packetizer.packetize(payload.data(), payload.size(),
                         proto::MediaKind::Audio, 1000, false, 1, packets);
    const uint8_t* inner = nullptr;
    size_t inner_len = 0;
    size_t consumed = 0;
    check(proto::Packetizer::strip_length_prefix(
              packets[0].data(), packets[0].size(), &consumed, &inner,
              &inner_len) &&
              inner_len + 4 == packets[0].size(),
          "TCP length prefix strip");
  }

  // RTP 封装。
  {
    proto::PacketizeOptions options;
    options.format = proto::WireFormat::RtpAvc;
    options.max_payload = 200;
    proto::Packetizer packetizer(options);
    std::vector<std::vector<uint8_t>> packets;
    packetizer.packetize(aus[0].data, aus[0].size, proto::MediaKind::Video,
                         90000, true, 0, packets);
    check(packets.size() > 1, "RTP large AU fragments");
    proto::Depacketizer depacketizer(proto::WireFormat::RtpAvc);
    bool fu_seen = false;
    bool all_ok = true;
    uint16_t expected = 0;
    for (const auto& packet : packets) {
      proto::ParsedPacket parsed;
      std::string perr;
      if (!depacketizer.parse(packet.data(), packet.size(), &parsed, &perr)) {
        all_ok = false;
        break;
      }
      if (parsed.rtp_seq != expected++) all_ok = false;
      if (parsed.split_position == 1 || !(parsed.flags & proto::kFlagAuStart)) {
        fu_seen = true;
      }
      if ((parsed.flags & proto::kFlagAuStart) && parsed.payload_len > 1) {
        // 首个分片的 FU 头需携带原 NAL 类型
      }
    }
    check(all_ok, "RTP seq continuity");
    check(fu_seen, "RTP FU-A fragmentation used");
    check(packets.back().size() >= 12, "RTP packet size sanity");
  }

  // WAV 解析。
  {
    const std::vector<uint8_t> wav = make_wav_pcm16(48000, 2, 480);
    proto::wav::WavInfo info;
    std::string werr;
    check(proto::wav::parse(wav.data(), wav.size(), &info, &werr),
          "WAV parse (" + werr + ")");
    check(info.sample_rate == 48000 && info.channels == 2 &&
              info.bits_per_sample == 16,
          "WAV fmt fields");
    check(info.data_size == 480 * 2 * 2, "WAV data size");
    std::vector<int16_t> samples;
    check(proto::wav::to_s16le(info, &samples, &werr), "WAV to s16le");
    check(samples.size() == 480 * 2, "WAV sample count");
  }

  // 封装格式名称。
  {
    proto::WireFormat format;
    check(proto::wire_format_from_name("legacy-v1", &format) &&
              format == proto::WireFormat::LegacyEzV1,
          "wire format name legacy-v1");
    check(proto::wire_format_from_name("rtp", &format) &&
              format == proto::WireFormat::RtpAvc,
          "wire format name rtp");
    check(!proto::wire_format_from_name("bogus", &format),
          "wire format rejects unknown");
  }

  std::printf("\nSELFTEST: %s (%d checks, %d failures)\n",
              g_failures == 0 ? "PASS" : "FAIL", g_checks, g_failures);
  return g_failures == 0 ? 0 : 1;
}

int run_audio_probe() {
  using namespace usbdisplay;
  std::printf("== PulseAudio 输出 ==\n");
  std::unique_ptr<audio::PulseDiscovery> discovery =
      audio::make_libpulse_discovery();
  std::vector<audio::PulseSinkInfo> sinks;
  std::string err;
  if (!discovery->list_sinks(&sinks, &err)) {
    std::printf("  sink 查询失败：%s\n", err.c_str());
    return 1;
  }
  for (const auto& sink : sinks) {
    std::printf("  sink：%s\n", sink.name.c_str());
  }
  std::string default_sink;
  if (discovery->get_default_sink(&default_sink, &err)) {
    std::printf("  默认 sink：%s\n", default_sink.c_str());
  } else {
    std::printf("  默认 sink 查询失败：%s\n", err.c_str());
  }
  const std::string usb_sink = audio::select_usb_sink(sinks);
  std::printf("  USB sink：%s\n",
              usb_sink.empty() ? "未找到" : usb_sink.c_str());
  std::unique_ptr<audio::PulseCardControl> card_control =
      audio::make_libpulse_card_control();
  std::vector<audio::PulseCardInfo> cards;
  if (card_control->list_cards(&cards, &err)) {
    for (const auto& card : cards) {
      std::printf("  声卡：%s profile=%s\n", card.name.c_str(),
                  card.active_profile.c_str());
    }
  } else {
    std::printf("  声卡查询失败：%s\n", err.c_str());
  }
  std::printf("  默认输出未修改\n");
  return 0;
}

void print_usage() {
  std::printf(
      "usage: usbdisplay_diag [--offline] [--vid 0x1de1] [--pid 0xf201]\n"
      "                        [--hidraw /dev/hidrawN] [--host 192.168.203.1]\n"
      "                        [--port N] [--trace off|error|warn|info|debug|wire]\n"
      "\n"
      "  --offline   run the protocol self-test (no hardware needed)\n"
      "  --probe     full live probe: USB/HID report sizes, ALSA, reachability\n"
      "  --probe-json  also send one heartbeat and print device JSON replies\n"
      "  --probe-audio list PulseAudio sinks and the selected USB sink\n"
      "  --vid/--pid override the expected USB IDs\n"
      "  --hidraw    restrict diagnostics to one hidraw node\n"
      "  --host      receiver host for reachability probe\n");
}

void dump_usb_device(const usbdisplay::transport::UsbDeviceInfo& info) {
  std::printf("\nUSB device %04x:%04x bus %u addr %u bcdDevice 0x%04x\n",
              info.vid, info.pid, info.bus, info.address, info.bcd_device);
  if (!info.manufacturer.empty()) {
    std::printf("  manufacturer: %s\n", info.manufacturer.c_str());
  }
  if (!info.product.empty()) {
    std::printf("  product     : %s\n", info.product.c_str());
  }
  if (!info.serial.empty()) {
    std::printf("  serial      : %s\n", info.serial.c_str());
  }
  std::printf("  HID interface: %s\n", info.has_hid_interface ? "yes" : "no");
  for (const auto& iface : info.interfaces) {
    std::printf("  interface %d alt %d class 0x%02x/0x%02x/0x%02x %s\n",
                iface.number, iface.alternate, iface.klass, iface.subclass,
                iface.protocol,
                iface.claimed_by_kernel_driver ? "(kernel driver bound)" : "");
    for (const auto& ep : iface.endpoints) {
      std::printf(
          "    endpoint 0x%02x %s %s max_packet=%u interval=%u\n", ep.address,
          usbdisplay::transport::endpoint_direction(ep.address),
          usbdisplay::transport::endpoint_type_name(ep.transfer_type),
          ep.max_packet_size, ep.interval);
    }
  }
}

void dump_hidraw(const std::vector<usbdisplay::transport::HidrawInfo>& list,
                 uint16_t vid, uint16_t pid) {
  std::printf("\nHID raw devices (/dev/hidraw*): %zu\n", list.size());
  for (const auto& info : list) {
    const bool target = (info.vid == vid && info.pid == pid);
    std::printf("  %s %s %04x:%04x iface %d \"%s\" %s%s\n", info.path.c_str(),
                target ? "[TARGET]" : "        ", info.vid, info.pid,
                info.interface_number, info.name.c_str(),
                info.readable ? "r" : "-", info.writable ? "w" : "-");
    if (target && !info.writable) {
      std::printf(
          "    NOTE: not writable by this user. For unprivileged access add a\n"
          "    udev rule (do not install as part of this project):\n"
          "      SUBSYSTEM==\"hidraw\", ATTRS{idVendor}==\"%04x\", "
          "ATTRS{idProduct}==\"%04x\", MODE=\"0666\"\n",
          vid, pid);
    }
    if (target) {
      usbdisplay::transport::HidrawDevice device;
      std::string derr;
      if (device.open(info.path, &derr)) {
        const auto descriptor = device.report_descriptor(&derr);
        usbdisplay::transport::HidReportSizes sizes;
        if (!descriptor.empty() &&
            usbdisplay::transport::parse_hid_report_descriptor(descriptor,
                                                               &sizes, &derr)) {
          for (const auto& kv : sizes.input) {
            std::printf("    report id %u: input %zu data bytes (hidraw buffer "
                        "%zu)\n",
                        kv.first, kv.second, kv.second + 1);
          }
          for (const auto& kv : sizes.output) {
            std::printf("    report id %u: output %zu data bytes (hidraw buffer "
                        "%zu)\n",
                        kv.first, kv.second, kv.second + 1);
          }
          for (const auto& kv : sizes.feature) {
            std::printf("    report id %u: feature %zu data bytes\n", kv.first,
                        kv.second);
          }
          const int iface = info.interface_number;
          if (iface == 0) {
            if (sizes.output.count(1) && sizes.output[1] + 1 == 4096) {
              std::printf("    [ok] IF0 (control) report id 1, 4096-byte buffer\n");
            } else {
              std::printf("    [warn] IF0 report 1 is not the expected 4096 "
                          "byte buffer\n");
            }
          } else if (iface == 1) {
            if (sizes.output.count(2) && sizes.output[2] + 1 == 12288) {
              std::printf(
                  "    [ok] IF1 (video) descriptor report id 2, 12288 B; "
                  "verified protocol writes 4096-byte URBs via libusb\n");
            } else {
              std::printf("    [warn] IF1 report 2 is not the expected 12288 "
                          "byte buffer\n");
            }
          }
        } else {
          std::printf("    report descriptor parse failed: %s\n", derr.c_str());
        }
      } else {
        std::printf("    open for descriptor failed: %s\n", derr.c_str());
      }
    }
  }
}

void dump_alsa() {
  std::printf("\n== ALSA (device USB audio) ==\n");
  std::vector<uint8_t> cards;
  std::string err;
  if (usbdisplay::read_file("/proc/asound/cards", &cards, &err)) {
    const std::string text(cards.begin(), cards.end());
    std::printf("%s", text.c_str());
    if (text.find("USBDISPLAY") == std::string::npos &&
        text.find("USB_DISPLAY") == std::string::npos) {
      std::printf("  no USB_DISPLAY sound card found\n");
    } else {
      std::printf(
          "  audio path: plughw:USBDISPLAY,0 (libasound, 48 kHz stereo s16le)\n");
    }
  } else {
    std::printf("  /proc/asound/cards unavailable\n");
  }
  std::vector<uint8_t> pcm;
  if (usbdisplay::read_file("/proc/asound/pcm", &pcm, &err)) {
    const std::string text(pcm.begin(), pcm.end());
    size_t pos = 0;
    while (pos < text.size()) {
      const size_t end = text.find('\n', pos);
      const std::string line =
          text.substr(pos, end == std::string::npos ? std::string::npos
                                                    : end - pos);
      if (line.find("USB") != std::string::npos ||
          line.find("USBDISPLAY") != std::string::npos) {
        std::printf("  pcm: %s\n", line.c_str());
      }
      if (end == std::string::npos) break;
      pos = end + 1;
    }
  }
}

bool run_json_probe(const std::string& explicit_path, uint8_t report_id,
                    size_t report_size) {
  using namespace usbdisplay;
  std::printf("\n== JSON heartbeat probe ==\n");
  std::string path = explicit_path;
  std::string err;
  if (path.empty()) {
    transport::HidrawInfo info;
    if (!transport::find_hidraw_by_interface(kDefaultVid, kDefaultPid, 0, &info,
                                             &err)) {
      std::printf("  [warn] no control hidraw: %s\n", err.c_str());
      return false;
    }
    path = info.path;
  }
  transport::HidrawDevice device;
  if (!device.open(path, &err)) {
    std::printf("  [warn] open %s: %s\n", path.c_str(), err.c_str());
    return false;
  }
  // 心跳必须携带数字形式的会话编号，否则设备不回复。
  const std::string session_id =
      std::to_string((mono_ms() % 900000ULL) + 100000ULL);
  const std::string heartbeat =
      "{\"jsonrpc\":\"2.0\", \"method\": \"heartbeat\", \"params\": "
      "{\"session-id\": \"" + session_id + "\"}}";
  std::vector<std::vector<uint8_t>> reports;
  if (!hid::build_reports(
          reinterpret_cast<const uint8_t*>(heartbeat.data()), heartbeat.size(),
          report_id, report_size, true, 0, 0, &reports, &err)) {
    std::printf("  [fail] framing: %s\n", err.c_str());
    return false;
  }
  for (const auto& report : reports) {
    if (!device.write_raw(report.data(), report.size(), &err)) {
      std::printf("  [fail] write: %s\n", err.c_str());
      return false;
    }
  }
  std::printf("  heartbeat sent on %s (report id %u, %zu bytes, session %s)\n",
              path.c_str(), report_id, report_size, session_id.c_str());
  std::vector<uint8_t> buffer(report_size + 1);
  int replies = 0;
  int heartbeats = 1;
  const uint64_t start = mono_ms();
  // 设备开始投屏时可能短暂忙碌，因此重试一次心跳。
  while (mono_ms() - start < 4500) {
    if (replies == 0 && heartbeats < 2 && mono_ms() - start > 1800) {
      for (const auto& report : reports) {
        if (!device.write_raw(report.data(), report.size(), &err)) break;
      }
      ++heartbeats;
      std::printf("  heartbeat retry #%d\n", heartbeats - 1);
    }
    const int n = device.read_report(buffer.data(), buffer.size(), 200, &err);
    if (n <= 0) continue;
    if (n < static_cast<int>(hid::kOuterHeaderSize)) continue;
    const size_t data_len = static_cast<size_t>(n);
    hid::OuterHeader outer;
    if (!hid::parse_outer_header(buffer.data(), data_len, &outer, &err)) continue;
    size_t take = outer.length;
    if (take > data_len - hid::kOuterHeaderSize) {
      take = data_len - hid::kOuterHeaderSize;
    }
    std::string payload(
        reinterpret_cast<const char*>(buffer.data() + hid::kOuterHeaderSize),
        take);
    if (payload.empty()) continue;
    ++replies;
    std::printf("  <- [rid %u len %zu] %s\n", buffer[0], take,
                payload.substr(0, 512).c_str());
  }
  if (replies == 0) {
    std::printf("  [warn] no reply within 2s (device may be in projection "
                "mode; replug USB to reset)\n");
    return false;
  }
  std::printf("  [ok] %d device message(s) received\n", replies);
  return true;
}

int run_hardware_diag(uint16_t vid, uint16_t pid, const std::string& hidraw,
                      const std::string& host, uint16_t port,
                      bool probe_json) {
  using namespace usbdisplay;
  std::printf("%s\n\n== USB devices (libusb) ==\n", version_string());
  std::string err;
  const auto devices = transport::enumerate_usb(&err);
  if (!err.empty()) std::printf("libusb: %s\n", err.c_str());
  transport::UsbDeviceInfo target;
  bool found = false;
  for (const auto& info : devices) {
    if (info.vid == vid && info.pid == pid) {
      found = true;
      target = info;
    }
  }
  std::printf("total USB devices: %zu\n", devices.size());
  for (const auto& info : devices) {
    if (info.vid == vid && info.pid == pid) continue;
    std::printf("  %04x:%04x %s\n", info.vid, info.pid, info.product.c_str());
  }
  if (found) {
    dump_usb_device(target);
  } else {
    std::printf("\nTARGET %04x:%04x NOT FOUND on USB\n", vid, pid);
  }

  std::printf("\n== HID raw ==\n");
  const auto hids = transport::enumerate_hidraw(&err);
  if (!err.empty()) std::printf("hidraw: %s\n", err.c_str());
  dump_hidraw(hids, vid, pid);
  if (!hidraw.empty()) {
    std::printf("explicit hidraw path requested: %s\n", hidraw.c_str());
    transport::HidrawDevice device;
    if (device.open(hidraw, &err)) {
      transport::HidrawInfo info;
      device.get_info(&info, nullptr);
      std::printf("  opened %s: %s %04x:%04x iface %d\n", hidraw.c_str(),
                  info.name.c_str(), info.vid, info.pid, info.interface_number);
      const auto desc = device.report_descriptor(&err);
      if (!desc.empty()) {
        std::printf("  report descriptor (%zu bytes):\n", desc.size());
        std::printf("%s", hexdump(desc.data(), desc.size(), 256).c_str());
      } else {
        std::printf("  report descriptor unavailable: %s\n", err.c_str());
      }
    } else {
      std::printf("  open failed: %s\n", err.c_str());
    }
  }

  dump_alsa();

  if (probe_json) run_json_probe(hidraw, 1, 4096);

  std::printf("\n== USB HID transport mapping (verified protocol) ==\n");
  std::printf("  IF0 control : report id 1, 4096-byte hidraw buffer, JSON-RPC\n");
  std::printf("  IF1 video   : EP2 OUT via libusb, exactly 4096-byte URBs\n");
  std::printf("                (the HID descriptor says 12288; the app writes\n");
  std::printf("                 4096-byte URBs and so must we)\n");
  std::printf("  IF2/IF3     : USB audio (ALSA plughw:USBDISPLAY,0)\n");
  std::printf("  IF4         : mass storage virtual CD (ignored)\n");
  if (found) {
    std::string access_err;
    const bool accessible = transport::usb_device_accessible(vid, pid,
                                                             &access_err);
    std::printf("  libusb access: %s\n",
                accessible ? "ok (video EP2 usable)"
                           : ("DENIED - " + access_err).c_str());
  }

  std::printf("\n== Network interfaces ==\n");
  const auto net = transport::enumerate_net_interfaces(&err);
  for (const auto& ni : net) {
    if (ni.name == "lo") continue;
    std::printf("  %-14s %s mtu %u mac %s\n", ni.name.c_str(),
                ni.is_up ? "UP" : "DOWN", ni.mtu, ni.mac.c_str());
    for (const auto& ip : ni.ipv4) {
      std::printf("      ipv4 %s%s\n", ip.c_str(),
                  transport::ip_in_device_subnet(ip) ? "  <-- device subnet"
                                                     : "");
    }
    for (const auto& ip : ni.ipv6) std::printf("      ipv6 %s\n", ip.c_str());
  }
  bool device_net = false;
  for (const auto& ni : net) {
    for (const auto& ip : ni.ipv4) {
      if (transport::ip_in_device_subnet(ip)) device_net = true;
    }
  }
  if (!device_net) {
    std::printf(
        "  no local interface in 192.168.203.0/24 or 192.168.168.0/24; if the\n"
        "  receiver exposes a USB network gadget, check for a new interface\n"
        "  (rndis_host/cdc_ether) and its DHCP address.\n");
  }

  std::printf("\n== Receiver reachability (%s) ==\n", host.c_str());
  std::vector<uint16_t> probe_ports = {port, 8000, 80, 8080, 9000};
  std::sort(probe_ports.begin(), probe_ports.end());
  probe_ports.erase(std::unique(probe_ports.begin(), probe_ports.end()),
                    probe_ports.end());
  for (uint16_t p : probe_ports) {
    const bool up = transport::tcp_probe(host, p, 400);
    std::printf("  tcp %s:%-5u %s\n", host.c_str(), p, up ? "open" : "closed");
  }

  std::printf("\n== Summary ==\n");
  std::printf("  USB target      : %s\n", found ? "present" : "not found");
  std::printf("  HID control     : %s\n",
              found ? "see /dev/hidraw above"
                    : "unknown (device not enumerated)");
  std::printf("  Device network  : %s\n",
              device_net ? "interface detected" : "not detected");
  std::printf(
      "\nNext steps (real USB HID transport):\n"
      "  1. usbdisplay_diag --probe-json   (HID mapping + heartbeat test)\n"
      "  2. usbdisplay_json --seconds 10   (JSON session: events/heartbeats)\n"
      "  3. usbdisplay_probe --seconds 5   (read device HID input reports)\n"
      "  4. usbdisplay_send --live --h264 stream.h264\n"
      "     (verified control handshake + libusb 4096-byte EP2 URBs)\n"
      "  5. usbdisplay_audio --tone-hz 440 --seconds 3\n");
  return 0;
}

const char* trace_names[] = {"off", "error", "warn", "info", "debug", "wire"};

bool parse_trace(const std::string& text, usbdisplay::TraceLevel* out) {
  for (int i = 0; i < 6; ++i) {
    if (text == trace_names[i]) {
      *out = static_cast<usbdisplay::TraceLevel>(i);
      return true;
    }
  }
  return false;
}

}  // namespace

int main(int argc, char** argv) {
  using namespace usbdisplay;
  bool offline = false;
  bool probe = false;
  bool probe_json = false;
  bool probe_audio = false;
  uint16_t vid = kDefaultVid;
  uint16_t pid = kDefaultPid;
  std::string hidraw;
  std::string host = kDefaultDeviceHost;
  uint16_t port = proto::kProvisionalMediaTcpPort;
  TraceLevel trace = TraceLevel::Warn;

  for (int i = 1; i < argc; ++i) {
    const std::string arg = argv[i];
    auto next = [&](std::string* out) -> bool {
      if (i + 1 >= argc) return false;
      *out = argv[++i];
      return true;
    };
    if (arg == "--offline" || arg == "-o") {
      offline = true;
    } else if (arg == "--probe") {
      probe = true;
    } else if (arg == "--probe-json") {
      probe = true;
      probe_json = true;
    } else if (arg == "--probe-audio") {
      probe_audio = true;
    } else if (arg == "--help" || arg == "-h") {
      print_usage();
      return 0;
    } else if (arg == "--vid") {
      std::string value;
      if (!next(&value)) return 2;
      vid = static_cast<uint16_t>(std::strtoul(value.c_str(), nullptr, 0));
    } else if (arg == "--pid") {
      std::string value;
      if (!next(&value)) return 2;
      pid = static_cast<uint16_t>(std::strtoul(value.c_str(), nullptr, 0));
    } else if (arg == "--hidraw") {
      if (!next(&hidraw)) return 2;
    } else if (arg == "--host") {
      if (!next(&host)) return 2;
    } else if (arg == "--port") {
      std::string value;
      if (!next(&value)) return 2;
      port = static_cast<uint16_t>(std::strtoul(value.c_str(), nullptr, 0));
    } else if (arg == "--trace") {
      std::string value;
      if (!next(&value) || !parse_trace(value, &trace)) return 2;
    } else {
      std::printf("unknown argument: %s\n", arg.c_str());
      print_usage();
      return 2;
    }
  }
  log::set_level(trace);
  if (offline) return run_offline_selftest();
  if (probe_audio) return run_audio_probe();
  if (probe) {
    std::printf("== full live probe ==\n");
  }
  return run_hardware_diag(vid, pid, hidraw, host, port, probe_json);
}
