// SPDX-License-Identifier: MIT
#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace usbdisplay {

// USB IDs of the Actions Micro USB Display receiver (HID control channel).
constexpr uint16_t kDefaultVid = 0x1DE1;
constexpr uint16_t kDefaultPid = 0xF201;
// Default IP of the receiver when it exposes a USB network interface
// (RNDIS/CDC-ECM). The other address seen in the Windows application is
// 192.168.168.1; see PROTO_FACTS.md.
constexpr const char* kDefaultDeviceHost = "192.168.203.1";

enum class Codec : uint8_t { H264 = 0, HEVC = 1 };

// Video wire framing variants (see README "live-test matrix").
enum class FrameMode : uint8_t {
  AppRaw = 0,    // app USB style: 16-byte outer header + raw Annex-B (no inner)
  AppInner = 1,  // Wi-Fi style: 16-byte outer header + 32/36-byte inner header
  Ugreen = 2,    // 14-byte LE outer header + 32-byte RRIM/TADV inner header
};

// Video stream description handed to the receiver.
struct VideoConfig {
  Codec codec = Codec::H264;
  uint16_t width = 1920;
  uint16_t height = 1080;
  uint16_t fps = 60;  // verified capture runs at 60 fps
  uint32_t bitrate_bps = 8'000'000;
  bool fullscreen = false;
};

enum class AudioFormat : uint8_t {
  PCM_S16LE = 0,  // interleaved signed 16-bit little-endian
  PCM_F32LE = 1,  // interleaved 32-bit float little-endian
};

struct AudioConfig {
  uint32_t sample_rate = 48000;
  uint16_t channels = 2;
  AudioFormat format = AudioFormat::PCM_S16LE;
  // Packetization duration for PCM->media packets.
  uint16_t frame_ms = 20;
};

enum class TraceLevel : int {
  Off = 0,
  Error = 1,
  Warn = 2,
  Info = 3,
  Debug = 4,
  Wire = 5,  // dump every packet (hex)
};

enum class LinkMode : uint8_t {
  Auto = 0,             // HID control if present + IP media channel
  IpOnly = 1,           // no HID control, media over TCP/UDP only
  UsbOnly = 2,          // HID control only (diagnostic / handshake only)
};

// How media bytes are transported to the receiver.
enum class MediaTransport : uint8_t {
  Tcp = 0,
  Udp = 1,
  Loopback = 2,  // in-process sink, for tests / dry-run
  Null = 3,      // count frames, send nothing
  UsbHid = 4,    // real USB path: HID report channel on IF1 (video) + IF0 (control)
};

// Selects and locates the receiver.
struct DeviceSelector {
  LinkMode mode = LinkMode::Auto;
  uint16_t vid = kDefaultVid;
  uint16_t pid = kDefaultPid;
  // Explicit /dev/hidrawN path for the control channel; empty = auto-discover
  // by matching vid/pid in /sys/class/hidraw.
  std::string hidraw_path;
  // Receiver address for the media channel (USB network gadget or Wi-Fi).
  std::string host = kDefaultDeviceHost;
  // Optional local bind address / interface name (e.g. "192.168.203.2").
  std::string bind_address;
  // 0 = use protocol defaults.
  uint16_t control_port = 0;
  uint16_t video_port = 0;
  uint16_t audio_port = 0;
  MediaTransport media_transport = MediaTransport::Tcp;
  bool use_udp_audio = true;
  // Media wire format: "legacy-v1" (provisional EZDP header) or "rtp".
  std::string wire_format = "legacy-v1";
  // Max payload bytes per media packet (keep below path MTU for UDP).
  size_t max_payload = 1400;
  int control_timeout_ms = 3000;
  int connect_timeout_ms = 3000;

  // ---- USB HID mode (the real device transport) ----
  // Explicit /dev/hidraw nodes. Empty = auto-discover by USB interface number
  // (IF0 = control, IF1 = video) from /sys/class/hidraw.
  std::string control_hidraw_path;
  std::string video_hidraw_path;
  // HID report framing. Report ID and total report length must match the
  // device's HID report descriptor; defaults are the values measured on the
  // real 1de1:f201 receiver (IF0: id 1/4096 B, IF1: id 2/12288 B).
  uint8_t control_report_id = 1;
  size_t control_report_size = 4096;
  uint8_t video_report_id = 2;
  // Verified video transport: libusb EP2 OUT writes of exactly 4096-byte URBs
  // (despite the HID report descriptor advertising 12288). The name is kept
  // for API compatibility; --report-size overrides the URB size.
  size_t video_report_size = 4096;
  // Provisional: repeat the 16-byte outer header in every report instead of
  // only the first report of a message.
  bool report_header_per_chunk = false;
  // Inner 32-byte message header fields for video frames (provisional; the
  // values are being confirmed from the reverse engineering).
  uint32_t video_msg_type = 0;   // [8]  message type
  uint32_t video_field_1 = 1;    // [16] constant 1
  uint32_t video_field_a = 0;    // [20]
  uint32_t video_field_b = 0;    // [24]
  uint32_t video_field_c = 0;    // [28]
  bool video_magic_1000 = true;  // [12] = 0x1000, [14] = 0
  // Optional smaller payload chunk per report (0 = report_size - 16).
  size_t video_chunk_payload = 0;
  // Video framing mode and mode-specific fields.
  // DEPRECATED (pre-verified-protocol experiments; ignored by the USB HID
  // path, which now always uses the verified 4096-byte URB framing).
  FrameMode frame_mode = FrameMode::AppRaw;
  // DEPRECATED
  bool video_inner_payload_len = false;
  uint8_t ugreen_type = 0;
  uint16_t ugreen_flags = 0;
  uint32_t ugreen_fraginfo = 0;
  std::string ugreen_magic = "RRIM";
  uint32_t video_outer_param3 = 0;  // app mode header param3 (a3)
  uint32_t video_outer_param4 = 0;  // app mode header param4 (a4)
  // When true, open()/start() fail if no IF1 video hidraw node is found
  // (used by --live so a missing device is an error, not a silent null sink).
  bool require_video_hidraw = false;
  // Do not open a control channel at all (video-only live streaming).
  bool disable_control = false;
  // When non-empty in HID mode, write the reports to this dump file instead
  // of /dev/hidraw (offline --dry-run). Same for the control channel.
  // USB mode: when non-empty, write the exact 4096-byte video URBs to this
  // dump file instead of libusb (offline --dry-run; fake_device --usb-dump).
  std::string report_dump_path;
  std::string control_report_dump_path;
  // libusb transfer type for the video endpoint: auto|interrupt|bulk.
  std::string usb_transfer = "auto";
  // The receiver firmware only accepts a fresh session. When startup/handshake
  // fails (no isAllowed, no app-launched reply, claim busy) reset and
  // re-enumerate the USB device automatically instead of requiring a replug.
  bool auto_reset = true;
  int auto_reset_retries = 3;    // reset+retry cycles after the first attempt
  int auto_reset_wait_ms = 5000; // wait for re-enumeration / hidraw node
  // Optional reset when closing (off by default: the next run auto-resets if
  // needed).
  bool reset_on_exit = false;
  // ALSA playback for the device's USB audio interface (IF2/IF3). Empty
  // string disables audio even if compiled in.
  std::string alsa_device = "plughw:USBDISPLAY,0";

  // Legacy HID control-channel framing knobs (unused by the report path).
  uint16_t hid_report_size = 64;
  uint8_t hid_report_id = 0;
  bool hid_feature_report = false;
  bool hid_chunked = false;

  // Emit a legacy stream-config packet (codec/resolution/audio JSON) on the
  // media channel at start(). Disable if it confuses the receiver.
  bool send_stream_config = true;
  // When true, a failed control handshake makes start() fail.
  bool require_handshake = false;
  // Send the provisional control handshake (common.* JSON-RPC methods) at
  // start(). Off by default for the live HID path until the sequence is
  // confirmed; enable explicitly with --handshake.
  bool send_handshake = false;
  // ALSA audio playback (real device USB audio interface) toggles.
  bool enable_audio = true;
  bool require_audio = false;

  static DeviceSelector usb(uint16_t vid = kDefaultVid, uint16_t pid = kDefaultPid);
  static DeviceSelector network(const std::string& host);
};

// Cumulative counters; snapshot obtained via Controller::stats().
struct Stats {
  uint64_t video_frames = 0;
  uint64_t video_bytes = 0;
  uint64_t video_packets = 0;
  uint64_t video_keyframes = 0;
  uint64_t audio_frames = 0;
  uint64_t audio_bytes = 0;
  uint64_t audio_packets = 0;
  uint64_t control_tx = 0;
  uint64_t control_rx = 0;
  uint64_t send_errors = 0;
  uint64_t dropped_packets = 0;   // sink backpressure / EAGAIN
  uint64_t heartbeat_tx = 0;
  uint64_t heartbeat_failures = 0;
  uint64_t last_video_pts_us = 0;
  uint64_t last_audio_pts_us = 0;
  uint64_t uptime_ms = 0;
};

enum class EventType : uint8_t {
  DeviceFound,
  Opened,
  Configured,
  StreamingStarted,
  StreamingStopped,
  KeyframeRequested,   // caller should force the next frame to be an IDR
  ControlMessage,      // JSON-RPC notification received from receiver
  WirePacket,
  Error,
};

struct Event {
  EventType type = EventType::Error;
  std::string message;   // human readable
  std::string method;    // for ControlMessage: JSON-RPC method
  std::string json;      // for ControlMessage: raw params/result JSON
  int code = 0;          // optional error code
};

using EventCallback = std::function<void(const Event&)>;
using TraceSink = std::function<void(TraceLevel, const std::string&)>;

}  // namespace usbdisplay
