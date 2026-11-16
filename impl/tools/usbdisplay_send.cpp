// SPDX-License-Identifier: MIT
// usbdisplay_send - push Annex-B H.264 (and optional WAV audio) to the
// Actions Micro USB Display receiver using the *verified* protocol
// (USB_PROTOCOL_VERIFIED.md / 1.pcapng).
//
// Transport (USB mode):
//   control : /dev/hidraw IF0, HID report id 1, 4096-byte reports, JSON-RPC
//   video   : libusb EP2 OUT on interface 1, 4096-byte URBs
//   audio   : ALSA plughw:USBDISPLAY,0 (48 kHz stereo S16)
//
// The video message of one frame is: first URB with [0]=2, BE32 frame length
// (including Annex-B start codes), BE32 width/height at offsets 8/12, then the
// first 4080 H.264 bytes at offset 16; continuation URBs carry zeros in
// [1..15] and the next 4080 bytes at offset 16.
//
// Frame pacing follows the stream's real frame rate (SPS VUI
// num_units_in_tick/time_scale or --fps) on an absolute CLOCK_MONOTONIC base.
// The default is a single sequential pass; looping requires an IDR-first stream
// and restarts the session.
#include <algorithm>
#include <chrono>
#include <cmath>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#include "common/log.h"
#include "common/util.h"
#include "proto/h264_sps.h"
#include "proto/nal_scan.h"
#include "proto/wav.h"
#include "usbdisplay/device.h"
#include "usbdisplay/protocol.h"
#include "usbdisplay/version.h"

namespace {

volatile std::sig_atomic_t g_stop = 0;
void on_signal(int) { g_stop = 1; }

struct Options {
  std::string h264_path;
  std::string wav_path;
  std::string host = usbdisplay::kDefaultDeviceHost;
  std::string hidraw;
  std::string sink;  // empty = verified USB path; tcp/udp/loopback/null = IP
  std::string wire = "legacy-v1";
  std::string dump_reports;  // dry run: write the 4096-byte video URBs
  std::string control_dump_reports;
  std::string alsa_device = "plughw:USBDISPLAY,0";
  std::string usb_transfer = "auto";  // auto|interrupt|bulk
  std::vector<std::string> init_commands;
  uint16_t port = 0;
  uint16_t audio_port = 0;
  uint16_t width = 1920;
  uint16_t height = 1080;
  uint16_t fps = 0;  // 0 = derive from SPS VUI, fallback 60
  uint32_t bitrate = 8'000'000;
  size_t max_payload = 1400;      // IP path only
  size_t video_report_size = 4096;
  size_t control_report_size = 4096;
  uint8_t video_report_id = 2;
  uint8_t control_report_id = 1;
  int synthetic_frames = 0;
  int idr_interval = 30;
  int repeat = 1;
  bool loop = false;
  bool no_pace = false;
  bool live = false;
  bool no_control = false;
  bool no_config = false;
  bool no_audio = false;
  bool no_handshake = false;
  bool auto_reset = true;
  int reset_retries = 3;
  int reset_wait_ms = 5000;
  bool reset_on_exit = false;
  bool inject_aud = false;
  bool stats = false;
  double duration_sec = 0.0;
  usbdisplay::TraceLevel trace = usbdisplay::TraceLevel::Warn;
};

void print_usage() {
  std::printf(
      "usage: usbdisplay_send [options]\n"
      "  --live                 write real HID (control) + libusb EP2 (video)\n"
      "                         reports; the default is a safe offline dry run\n"
      "  --h264 FILE            Annex-B H.264 stream (start codes preserved)\n"
      "  --synthetic-frames N   generate N synthetic frames instead of a file\n"
      "  --idr-interval N       synthetic IDR interval (default 30)\n"
      "  --wav FILE             WAV audio played through the device USB audio\n"
      "  --dump-reports FILE    dry run: write the generated 4096-byte URBs\n"
      "  --control-dump FILE    dry run: write control HID reports to FILE\n"
      "  --hidraw PATH          IF0 control hidraw node (default: auto IF0)\n"
      "  --report-size N        video URB size (default 4096)\n"
      "  --report-id N          video report id byte (default 2)\n"
      "  --control-report-size N / --control-report-id N  (default 4096 / 1)\n"
      "  --usb-transfer MODE    auto|interrupt|bulk (default auto)\n"
      "  --no-handshake         skip the verified JSON handshake/isAllowed wait\n"
      "  --auto-reset           reset/re-enumerate on failure (default on)\n"
      "  --no-auto-reset        do not reset/re-enumerate the device on failure\n"
      "  --reset-retries N      auto-reset cycles (default 3)\n"
      "  --reset-wait-ms N      re-enumeration wait per reset (default 5000)\n"
      "  --reset-on-exit        reset the device on clean shutdown too\n"
      "  --init-json JSON       raw JSON command sent after start (repeatable)\n"
      "  --inject-aud           prepend a default AUD to AUs that lack one\n"
      "  --alsa-device DEV      ALSA PCM (default plughw:USBDISPLAY,0)\n"
      "  --no-audio             do not play WAV audio\n"
      "  --no-control           do not open the IF0 control channel\n"
      "  --sink tcp|udp|loopback|null   use the legacy Wi-Fi IP path\n"
      "  --wire legacy-v1|rtp   IP wire profile (default legacy-v1)\n"
      "  --host HOST / --port N / --audio-port N   IP path endpoints\n"
      "  --width/--height N     advertised video size (default 1920x1080)\n"
      "  --fps N                pacing frame rate (default: from SPS VUI)\n"
      "  --bitrate N            advertised bitrate bps (default 8000000)\n"
      "  --repeat N             play the input N times (default 1)\n"
      "  --loop                 replay until --duration/SIGINT (requires IDR)\n"
      "  --duration SEC         stop after SEC seconds\n"
      "  --no-pace              ignore frame pacing\n"
      "  --stats                print periodic stats\n"
      "  --trace LEVEL          off|error|warn|info|debug|wire\n");
}

bool parse_trace(const std::string& text, usbdisplay::TraceLevel* out) {
  static const char* names[] = {"off", "error", "warn", "info", "debug", "wire"};
  for (int i = 0; i < 6; ++i) {
    if (text == names[i]) {
      *out = static_cast<usbdisplay::TraceLevel>(i);
      return true;
    }
  }
  return false;
}

void append_nal(std::vector<uint8_t>* out, uint8_t header,
                const std::vector<uint8_t>& payload) {
  out->push_back(0);
  out->push_back(0);
  out->push_back(0);
  out->push_back(1);
  out->push_back(header);
  out->insert(out->end(), payload.begin(), payload.end());
}

void append_start_code(std::vector<uint8_t>* out) {
  out->push_back(0);
  out->push_back(0);
  out->push_back(0);
  out->push_back(1);
}

std::vector<uint8_t> make_synthetic_au(int index, int idr_interval,
                                       uint16_t width, uint16_t height) {
  using namespace usbdisplay;
  std::vector<uint8_t> au;
  // Verified streams start every frame with an AUD (00 00 00 01 09 10).
  append_start_code(&au);
  au.push_back(0x09);
  au.push_back(0x10);
  const bool idr = (idr_interval <= 0) || (index % idr_interval == 0);
  const size_t base = static_cast<size_t>(width) * height / 64;
  auto fill = [](size_t n, uint8_t value) {
    return std::vector<uint8_t>(n, value);
  };
  if (idr) {
    // A syntactically valid SPS with VUI 60 fps so the fps-autodetect path is
    // exercised offline too.
    const std::vector<uint8_t> sps =
        proto::h264::make_test_sps(width, height, 1, 120);
    append_start_code(&au);
    au.insert(au.end(), sps.begin(), sps.end());
    append_nal(&au, 0x68, fill(6, 0x22));  // PPS
    std::vector<uint8_t> slice = fill(base + 700, 0xCD);
    slice[0] = 0x88;  // first_mb_in_slice ue(v) == 0
    append_nal(&au, 0x65, slice);  // IDR
  } else {
    append_nal(&au, 0x68, fill(6, 0x22));  // PPS
    std::vector<uint8_t> slice = fill(base + 40, 0xAB);
    slice[0] = 0x88;
    append_nal(&au, 0x41, slice);  // non-IDR
  }
  return au;
}

bool has_start_code(const uint8_t* data, size_t len) {
  if (data == nullptr || len < 3) return false;
  if (data[0] == 0 && data[1] == 0 && data[2] == 1) return true;
  return len >= 4 && data[0] == 0 && data[1] == 0 && data[2] == 0 &&
         data[3] == 1;
}

bool first_nal_type_is_aud(const uint8_t* data, size_t len) {
  std::vector<usbdisplay::proto::detail::NalRange> nals;
  if (!usbdisplay::proto::detail::scan_nal_units(data, len, &nals)) {
    return false;
  }
  return !nals.empty() && nals[0].type == 9;
}

std::vector<uint8_t> with_injected_aud(const uint8_t* data, size_t len) {
  std::vector<uint8_t> out;
  out.reserve(len + 6);
  append_start_code(&out);
  out.push_back(0x09);
  out.push_back(0x10);
  out.insert(out.end(), data, data + len);
  return out;
}

struct AudioData {
  std::vector<int16_t> samples;
  uint32_t sample_rate = 0;
  uint16_t channels = 0;
  size_t samples_per_chunk = 0;
  size_t total_chunks = 0;
  std::vector<int16_t> chunk(size_t index) const {
    std::vector<int16_t> out;
    if (samples_per_chunk == 0 || samples.empty() || total_chunks == 0) {
      return out;
    }
    const size_t start = (index % total_chunks) * samples_per_chunk;
    const size_t end = std::min(start + samples_per_chunk, samples.size());
    out.assign(samples.begin() + static_cast<long>(start),
               samples.begin() + static_cast<long>(end));
    return out;
  }
};

bool load_audio(const Options& options, AudioData* audio, std::string* err) {
  if (options.wav_path.empty()) return true;
  std::vector<uint8_t> bytes;
  if (!usbdisplay::read_file(options.wav_path, &bytes, err)) return false;
  usbdisplay::proto::wav::WavInfo info;
  if (!usbdisplay::proto::wav::parse(bytes.data(), bytes.size(), &info, err)) {
    return false;
  }
  if (!usbdisplay::proto::wav::to_s16le(info, &audio->samples, err)) {
    return false;
  }
  audio->sample_rate = info.sample_rate;
  audio->channels = info.channels;
  return true;
}

void print_stats(const usbdisplay::Stats& s) {
  std::printf(
      "stats: video_frames=%llu video_bytes=%llu video_packets=%llu "
      "keyframes=%llu audio_frames=%llu audio_bytes=%llu ctrl_tx=%llu "
      "hb=%llu hb_fail=%llu errors=%llu uptime=%llums\n",
      static_cast<unsigned long long>(s.video_frames),
      static_cast<unsigned long long>(s.video_bytes),
      static_cast<unsigned long long>(s.video_packets),
      static_cast<unsigned long long>(s.video_keyframes),
      static_cast<unsigned long long>(s.audio_frames),
      static_cast<unsigned long long>(s.audio_bytes),
      static_cast<unsigned long long>(s.control_tx),
      static_cast<unsigned long long>(s.heartbeat_tx),
      static_cast<unsigned long long>(s.heartbeat_failures),
      static_cast<unsigned long long>(s.send_errors),
      static_cast<unsigned long long>(s.uptime_ms));
}

// Absolute CLOCK_MONOTONIC scheduling with a sub-millisecond final sleep.
void sleep_until_us(uint64_t target_us) {
  for (;;) {
    const uint64_t now = usbdisplay::mono_us();
    if (target_us <= now) return;
    const uint64_t remaining = target_us - now;
    if (remaining <= 1500) {
      std::this_thread::sleep_for(std::chrono::microseconds(remaining));
      return;
    }
    std::this_thread::sleep_for(std::chrono::microseconds(remaining - 800));
  }
}

}  // namespace

int main(int argc, char** argv) {
  using namespace usbdisplay;
  Options options;
  for (int i = 1; i < argc; ++i) {
    const std::string arg = argv[i];
    auto next = [&](std::string* out) -> bool {
      if (i + 1 >= argc) return false;
      *out = argv[++i];
      return true;
    };
    auto next_num = [&](uint64_t* out) -> bool {
      std::string v;
      if (!next(&v)) return false;
      *out = std::strtoull(v.c_str(), nullptr, 0);
      return true;
    };
    if (arg == "--h264") {
      if (!next(&options.h264_path)) return 2;
    } else if (arg == "--wav" || arg == "--audio-wav") {
      if (!next(&options.wav_path)) return 2;
    } else if (arg == "--host") {
      if (!next(&options.host)) return 2;
    } else if (arg == "--hidraw") {
      if (!next(&options.hidraw)) return 2;
    } else if (arg == "--sink") {
      if (!next(&options.sink)) return 2;
    } else if (arg == "--wire") {
      if (!next(&options.wire)) return 2;
    } else if (arg == "--dump-reports") {
      if (!next(&options.dump_reports)) return 2;
    } else if (arg == "--control-dump") {
      if (!next(&options.control_dump_reports)) return 2;
    } else if (arg == "--alsa-device") {
      if (!next(&options.alsa_device)) return 2;
    } else if (arg == "--usb-transfer") {
      if (!next(&options.usb_transfer)) return 2;
    } else if (arg == "--init-json") {
      std::string v;
      if (!next(&v)) return 2;
      options.init_commands.push_back(v);
    } else if (arg == "--port") {
      uint64_t v = 0;
      if (!next_num(&v)) return 2;
      options.port = static_cast<uint16_t>(v);
    } else if (arg == "--audio-port") {
      uint64_t v = 0;
      if (!next_num(&v)) return 2;
      options.audio_port = static_cast<uint16_t>(v);
    } else if (arg == "--width") {
      uint64_t v = 0;
      if (!next_num(&v)) return 2;
      options.width = static_cast<uint16_t>(v);
    } else if (arg == "--height") {
      uint64_t v = 0;
      if (!next_num(&v)) return 2;
      options.height = static_cast<uint16_t>(v);
    } else if (arg == "--fps") {
      uint64_t v = 0;
      if (!next_num(&v)) return 2;
      options.fps = static_cast<uint16_t>(v);
    } else if (arg == "--bitrate") {
      uint64_t v = 0;
      if (!next_num(&v)) return 2;
      options.bitrate = static_cast<uint32_t>(v);
    } else if (arg == "--max-payload") {
      uint64_t v = 0;
      if (!next_num(&v)) return 2;
      options.max_payload = static_cast<size_t>(v);
    } else if (arg == "--report-size") {
      uint64_t v = 0;
      if (!next_num(&v)) return 2;
      options.video_report_size = static_cast<size_t>(v);
    } else if (arg == "--report-id") {
      uint64_t v = 0;
      if (!next_num(&v)) return 2;
      options.video_report_id = static_cast<uint8_t>(v);
    } else if (arg == "--control-report-size") {
      uint64_t v = 0;
      if (!next_num(&v)) return 2;
      options.control_report_size = static_cast<size_t>(v);
    } else if (arg == "--control-report-id") {
      uint64_t v = 0;
      if (!next_num(&v)) return 2;
      options.control_report_id = static_cast<uint8_t>(v);
    } else if (arg == "--synthetic-frames") {
      uint64_t v = 0;
      if (!next_num(&v)) return 2;
      options.synthetic_frames = static_cast<int>(v);
    } else if (arg == "--idr-interval") {
      uint64_t v = 0;
      if (!next_num(&v)) return 2;
      options.idr_interval = static_cast<int>(v);
    } else if (arg == "--repeat") {
      uint64_t v = 0;
      if (!next_num(&v)) return 2;
      options.repeat = static_cast<int>(v);
    } else if (arg == "--duration") {
      std::string v;
      if (!next(&v)) return 2;
      options.duration_sec = std::atof(v.c_str());
    } else if (arg == "--loop") {
      options.loop = true;
    } else if (arg == "--live" || arg == "--video-probe") {
      options.live = true;
    } else if (arg == "--no-control") {
      options.no_control = true;
    } else if (arg == "--no-config") {
      options.no_config = true;
    } else if (arg == "--no-audio") {
      options.no_audio = true;
    } else if (arg == "--no-handshake") {
      options.no_handshake = true;
    } else if (arg == "--no-auto-reset") {
      options.auto_reset = false;
    } else if (arg == "--reset-retries") {
      uint64_t v = 0;
      if (!next_num(&v)) return 2;
      options.reset_retries = static_cast<int>(v);
    } else if (arg == "--reset-wait-ms") {
      uint64_t v = 0;
      if (!next_num(&v)) return 2;
      options.reset_wait_ms = static_cast<int>(v);
    } else if (arg == "--reset-on-exit") {
      options.reset_on_exit = true;
    } else if (arg == "--inject-aud") {
      options.inject_aud = true;
    } else if (arg == "--no-pace") {
      options.no_pace = true;
    } else if (arg == "--stats") {
      options.stats = true;
    } else if (arg == "--trace") {
      std::string v;
      if (!next(&v) || !parse_trace(v, &options.trace)) return 2;
    } else if (arg == "--help" || arg == "-h") {
      print_usage();
      return 0;
    } else {
      std::fprintf(stderr, "unknown argument: %s\n", arg.c_str());
      print_usage();
      return 2;
    }
  }

  log::set_level(options.trace);
  std::signal(SIGINT, on_signal);
  std::signal(SIGTERM, on_signal);

  if (options.h264_path.empty() && options.synthetic_frames <= 0) {
    std::fprintf(stderr, "need --h264 FILE or --synthetic-frames N\n");
    print_usage();
    return 2;
  }

  // ---- load video and preserve Annex-B access units ----
  std::vector<uint8_t> stream;
  std::vector<std::vector<uint8_t>> owned_aus;
  std::vector<proto::annexb::AccessUnit> aus;
  std::string err;
  if (!options.h264_path.empty()) {
    if (!read_file(options.h264_path, &stream, &err)) {
      std::fprintf(stderr, "%s\n", err.c_str());
      return 1;
    }
    if (!proto::annexb::split_access_units(stream.data(), stream.size(), &aus,
                                           &err)) {
      std::fprintf(stderr, "%s\n", err.c_str());
      return 1;
    }
    // Root cause of the earlier black screen: AUs must keep the original
    // 00 00 00 01 start codes. Validate before doing anything else.
    for (size_t i = 0; i < aus.size(); ++i) {
      if (!has_start_code(aus[i].data, aus[i].size)) {
        std::fprintf(stderr,
                     "access unit %zu does not start with an Annex-B start "
                     "code; refusing to send (start codes are mandatory)\n",
                     i);
        return 1;
      }
    }
    log::info("loaded " + std::to_string(aus.size()) + " access units from " +
              options.h264_path);
    if (options.inject_aud) {
      size_t injected = 0;
      owned_aus.reserve(aus.size());
      for (const auto& au : aus) {
        if (!first_nal_type_is_aud(au.data, au.size)) {
          owned_aus.push_back(with_injected_aud(au.data, au.size));
          ++injected;
        } else {
          owned_aus.emplace_back(au.data, au.data + au.size);
        }
      }
      for (size_t i = 0; i < owned_aus.size(); ++i) {
        aus[i].data = owned_aus[i].data();
        aus[i].size = owned_aus[i].size();
      }
      if (injected > 0) {
        log::info("injected an AUD into " + std::to_string(injected) +
                  " access units that lacked one");
      }
    }
  } else {
    std::vector<std::vector<uint8_t>> synthetic;
    synthetic.reserve(static_cast<size_t>(options.synthetic_frames));
    for (int i = 0; i < options.synthetic_frames; ++i) {
      synthetic.push_back(make_synthetic_au(i, options.idr_interval,
                                            options.width, options.height));
    }
    for (const auto& au : synthetic) stream.insert(stream.end(), au.begin(), au.end());
    size_t offset = 0;
    for (size_t i = 0; i < synthetic.size(); ++i) {
      proto::annexb::AccessUnit desc;
      desc.data = stream.data() + offset;
      desc.size = synthetic[i].size();
      desc.keyframe = (options.idr_interval <= 0) ||
                      (static_cast<int>(i) % options.idr_interval == 0);
      aus.push_back(desc);
      offset += synthetic[i].size();
    }
    for (const auto& au : aus) {
      if (!has_start_code(au.data, au.size)) {
        std::fprintf(stderr, "internal error: synthetic AU without start code\n");
        return 1;
      }
    }
  }
  if (aus.empty()) {
    std::fprintf(stderr, "no access units to send\n");
    return 1;
  }

  // ---- frame rate: --fps > SPS VUI > 60 fallback ----
  double fps = 0.0;
  std::string fps_source = "--fps";
  if (options.fps != 0) {
    fps = options.fps;
  } else {
    double vui_fps = 0.0;
    proto::h264::SpsInfo sps_info;
    std::string fps_err;
    if (proto::h264::extract_fps_from_annexb(stream.data(), stream.size(),
                                             &vui_fps, &sps_info, &fps_err)) {
      fps = vui_fps;
      fps_source = "SPS VUI";
      log::info("SPS VUI: " + std::to_string(sps_info.width) + "x" +
                std::to_string(sps_info.height) + " " +
                std::to_string(vui_fps) + " fps (time_scale=" +
                std::to_string(sps_info.time_scale) + " num_units_in_tick=" +
                std::to_string(sps_info.num_units_in_tick) + ")");
    } else {
      fps = 60.0;
      fps_source = "default 60 (no VUI timing: " + fps_err + ")";
      log::warn("no SPS VUI frame rate (" + fps_err +
                "); falling back to 60 fps");
    }
  }
  if (fps <= 0.0) {
    std::fprintf(stderr, "invalid frame rate %f\n", fps);
    return 1;
  }
  if (options.repeat > 1 || options.loop) {
    if (!aus[0].keyframe) {
      std::fprintf(stderr,
                   "refusing to loop: the first access unit is not an IDR "
                   "frame (frame-number/decoder state would break); use a "
                   "single pass or an IDR-first stream\n");
      return 1;
    }
  }

  // ---- load audio ----
  AudioData audio;
  if (!load_audio(options, &audio, &err)) {
    std::fprintf(stderr, "%s\n", err.c_str());
    return 1;
  }
  if (!audio.samples.empty()) {
    const size_t frame_samples =
        static_cast<size_t>(audio.sample_rate) * 20 / 1000 * audio.channels;
    audio.samples_per_chunk = frame_samples;
    audio.total_chunks =
        (audio.samples.size() + frame_samples - 1) / frame_samples;
    log::info("loaded audio " + std::to_string(audio.samples.size() / audio.channels) +
              " samples @" + std::to_string(audio.sample_rate) + "Hz/" +
              std::to_string(audio.channels) + "ch");
  }

  // ---- select transport ----
  enum class RunMode { UsbLive, UsbDry, Ip };
  RunMode run_mode = RunMode::UsbDry;
  MediaTransport ip_transport = MediaTransport::Tcp;
  if (!options.sink.empty()) {
    run_mode = RunMode::Ip;
    if (options.sink == "tcp") {
      ip_transport = MediaTransport::Tcp;
    } else if (options.sink == "udp") {
      ip_transport = MediaTransport::Udp;
    } else if (options.sink == "loopback") {
      ip_transport = MediaTransport::Loopback;
    } else if (options.sink == "null") {
      ip_transport = MediaTransport::Null;
    } else {
      std::fprintf(stderr, "unknown --sink %s\n", options.sink.c_str());
      return 2;
    }
  } else if (options.live) {
    run_mode = RunMode::UsbLive;
  }
  if (run_mode != RunMode::UsbDry && !options.dump_reports.empty()) {
    std::fprintf(stderr, "--dump-reports is only used by the dry run; ignored\n");
    options.dump_reports.clear();
  }

  DeviceSelector selector;
  selector.vid = kDefaultVid;
  selector.pid = kDefaultPid;
  selector.host = options.host;
  selector.wire_format = options.wire;
  selector.max_payload = options.max_payload;
  selector.control_hidraw_path = options.hidraw;
  selector.control_report_id = options.control_report_id;
  selector.control_report_size = options.control_report_size;
  selector.video_report_id = options.video_report_id;
  selector.video_report_size = options.video_report_size;
  selector.usb_transfer = options.usb_transfer;
  selector.auto_reset = options.auto_reset;
  selector.auto_reset_retries = options.reset_retries;
  selector.auto_reset_wait_ms = options.reset_wait_ms;
  selector.reset_on_exit = options.reset_on_exit;
  selector.alsa_device = options.alsa_device;
  if (options.port != 0) selector.video_port = options.port;
  if (options.audio_port != 0) selector.audio_port = options.audio_port;

  switch (run_mode) {
    case RunMode::UsbLive:
      selector.media_transport = MediaTransport::UsbHid;
      selector.mode = LinkMode::Auto;
      selector.disable_control = options.no_control;
      selector.enable_audio = !options.wav_path.empty() && !options.no_audio;
      selector.send_handshake = !options.no_handshake;
      selector.require_handshake = !options.no_handshake;
      selector.send_stream_config = false;
      break;
    case RunMode::UsbDry:
      selector.media_transport = MediaTransport::UsbHid;
      selector.mode = LinkMode::IpOnly;  // never touch hardware
      selector.disable_control = true;
      selector.enable_audio = false;
      selector.send_handshake = false;
      selector.send_stream_config = false;
      selector.report_dump_path = options.dump_reports;
      selector.control_report_dump_path = options.control_dump_reports;
      break;
    case RunMode::Ip:
      selector.media_transport = ip_transport;
      selector.mode = options.no_control ? LinkMode::IpOnly : LinkMode::Auto;
      selector.disable_control = options.no_control;
      selector.hidraw_path = options.hidraw;
      selector.send_stream_config = !options.no_config;
      break;
  }

  VideoConfig video;
  video.codec = Codec::H264;
  video.width = options.width;
  video.height = options.height;
  video.fps = static_cast<uint16_t>(std::lround(fps));
  video.bitrate_bps = options.bitrate;

  AudioConfig audio_config;
  if (!audio.samples.empty()) {
    audio_config.sample_rate = audio.sample_rate;
    audio_config.channels = audio.channels;
    audio_config.format = AudioFormat::PCM_S16LE;
  }

  Controller controller;
  controller.set_trace(options.trace);
  controller.on_event([](const Event& event) {
    if (event.type == EventType::KeyframeRequested) {
      std::fprintf(stderr, "event: receiver requested a keyframe (SPS/PPS+IDR)\n");
    } else if (event.type == EventType::Error) {
      std::fprintf(stderr, "event: error: %s\n", event.message.c_str());
    } else if (log::enabled(TraceLevel::Info)) {
      std::fprintf(stderr, "event: %s\n", event.message.c_str());
    }
  });

  if (!controller.open(selector, &err)) {
    std::fprintf(stderr, "open failed: %s\n", err.c_str());
    return 1;
  }
  if (!controller.configure(video, audio_config, &err)) {
    std::fprintf(stderr, "configure failed: %s\n", err.c_str());
    controller.close();
    return 1;
  }
  if (!controller.start(&err)) {
    std::fprintf(stderr, "start failed: %s\n", err.c_str());
    controller.close();
    return 1;
  }
  const char* mode_name = run_mode == RunMode::UsbLive
                              ? "usb-live"
                              : (run_mode == RunMode::UsbDry ? "usb-dry" : "ip");
  std::printf(
      "sending: mode=%s media=%s video=%ux%u@%.3f fps (%s) %ukbps "
      "urb=%zuB/id%u handshake=%s auto-reset=%s\n",
      mode_name, controller.media_endpoint().c_str(), video.width, video.height,
      fps, fps_source.c_str(), video.bitrate_bps / 1000,
      options.video_report_size, options.video_report_id,
      run_mode == RunMode::UsbLive && !options.no_handshake ? "on" : "off",
      run_mode == RunMode::UsbLive && options.auto_reset
          ? ("on, " + std::to_string(options.reset_retries) + " retries").c_str()
          : "off");
  if (run_mode == RunMode::UsbDry) {
    std::printf(
        "dry run: %s\n",
        options.dump_reports.empty()
            ? "URBs are built in-process only (use --dump-reports FILE)"
            : "URBs are written to the dump file; validate with "
              "fake_device --usb-dump");
  }

  for (const auto& command : options.init_commands) {
    std::string response;
    if (!controller.send_json_command(command, &response, &err)) {
      std::fprintf(stderr, "init-json failed: %s\n", err.c_str());
    } else {
      std::printf("init-json sent: %s%s%s\n", command.c_str(),
                  response.empty() ? "" : " -> ", response.c_str());
    }
  }

  // ---- absolute-monotonic paced, single sequential pass by default ----
  const uint64_t start_us = mono_us();
  const double period_us = 1000000.0 / fps;
  uint64_t frames_sent = 0;
  uint64_t pass_frames = 0;
  int pass = 0;
  uint64_t audio_chunks_sent = 0;
  uint64_t last_stats_us = start_us;
  bool done = false;
  while (!done && !g_stop) {
    if (options.duration_sec > 0.0 &&
        (mono_us() - start_us) / 1e6 >= options.duration_sec) {
      break;
    }
    if (pass_frames >= aus.size()) {
      ++pass;
      if (!options.loop && pass >= options.repeat) {
        done = true;
        break;
      }
      // Looping requires an IDR-first stream and a fresh session so the
      // decoder reference state is rebuilt.
      if (options.loop) {
        log::info("loop: restarting session at pass " + std::to_string(pass));
        controller.stop();
        if (!controller.start(&err)) {
          std::fprintf(stderr, "loop restart failed: %s\n", err.c_str());
          break;
        }
      }
      pass_frames = 0;
    }
    const size_t au_index = static_cast<size_t>(pass_frames);
    const proto::annexb::AccessUnit& au = aus[au_index];
    const uint64_t target_us =
        start_us + static_cast<uint64_t>(std::llround(
                       static_cast<double>(frames_sent) * period_us));
    if (!options.no_pace) sleep_until_us(target_us);
    if (!controller.push_video(au.data, au.size, target_us - start_us,
                               au.keyframe, &err)) {
      std::fprintf(stderr, "push_video failed at frame %llu: %s\n",
                   static_cast<unsigned long long>(frames_sent), err.c_str());
      break;
    }
    ++frames_sent;
    ++pass_frames;

    // Audio: send chunks whose timeline is due before the next frame.
    if (!audio.samples.empty()) {
      const uint64_t next_frame_us = target_us +
                                     static_cast<uint64_t>(period_us);
      while (audio_chunks_sent * 20000ULL < next_frame_us) {
        const auto chunk = audio.chunk(audio_chunks_sent);
        if (chunk.empty() || g_stop) break;
        if (!controller.push_audio(chunk.data(), chunk.size() * sizeof(int16_t),
                                   audio_chunks_sent * 20000ULL, &err)) {
          std::fprintf(stderr, "push_audio failed: %s\n", err.c_str());
          break;
        }
        ++audio_chunks_sent;
      }
    }
    if (options.stats && mono_us() - last_stats_us > 1000000ULL) {
      last_stats_us = mono_us();
      print_stats(controller.stats());
    }
  }

  if (options.stats) print_stats(controller.stats());
  std::printf("\nfinal ");
  print_stats(controller.stats());
  controller.close();
  return 0;
}
