// SPDX-License-Identifier: MIT
// Public controller API.
//
// Typical use (caller owns capture + H.264 encoding; this library only
// packetizes and ships frames):
//
//   usbdisplay::Controller ctl;
//   std::string err;
//   auto sel = usbdisplay::DeviceSelector::usb();
//   sel.host = "192.168.203.1";
//   if (!ctl.open(sel, &err)) fail(err);
//   usbdisplay::VideoConfig v; // 1280x720@30, 4 Mbit/s
//   usbdisplay::AudioConfig a; // 48 kHz stereo s16le
//   if (!ctl.configure(v, a, &err)) fail(err);
//   ctl.start(&err);
//   ctl.push_video(annexb_data, annexb_len, pts_us, /*keyframe=*/true);
//   ctl.push_audio(pcm, pcm_bytes, pts_us);
//   ctl.close();
//
// All methods are thread-safe. push_* are blocking with an internal timeout;
// they return false and set err on transport failure. See README.md for the
// protocol assumptions and validation status.
#pragma once

#include <cstdint>
#include <memory>
#include <string>

#include "usbdisplay/types.h"

namespace usbdisplay {

class Controller {
 public:
  Controller();
  ~Controller();
  Controller(const Controller&) = delete;
  Controller& operator=(const Controller&) = delete;

  // Locate + open the receiver. In LinkMode::Auto this resolves the HID
  // control path (may be absent) and the media channel. When neither is
  // available, returns false unless a Loopback/Null transport is configured.
  bool open(const DeviceSelector& selector, std::string* err = nullptr);
  void close();
  bool is_open() const;

  // Negotiate stream parameters with the receiver (control channel only;
  // without a control channel this only records the configuration and the
  // caller must ensure the receiver is already in streaming mode).
  bool configure(const VideoConfig& video, const AudioConfig& audio,
                 std::string* err = nullptr);
  bool start(std::string* err = nullptr);
  bool stop();

  // Push one H.264/HEVC access unit in Annex-B form (start codes are kept
  // verbatim; the packetizer only splits into transport packets).
  bool push_video(const uint8_t* annexb, size_t len, uint64_t pts_us,
                  bool keyframe, std::string* err = nullptr);

  // Push PCM audio. Samples must match the configured AudioConfig. The data is
  // split into frame_ms chunks; pts_us is the presentation time of the first
  // byte of this buffer.
  bool push_audio(const void* pcm, size_t bytes, uint64_t pts_us,
                  std::string* err = nullptr);

  // Ask the receiver to drop/ignore data until the next IDR (also fires a
  // KeyframeRequested event so the caller can force its encoder). Returns
  // false if no control channel is available.
  bool request_keyframe(std::string* err = nullptr);

  // Send an arbitrary JSON-RPC request on the control channel (advanced use,
  // e.g. common.get_device_description). Returns the raw JSON response body.
  bool send_control(const std::string& method, const std::string& params_json,
                    std::string* response_json, std::string* err = nullptr);

  // Send a raw JSON text command on the HID control channel (IF0), exactly as
  // given (no JSON-RPC wrapping). Useful while iterating on the protocol.
  // When response_json is non-null it briefly waits for one device message.
  bool send_json_command(const std::string& json, std::string* response_json,
                         std::string* err = nullptr);

  // Block until one message arrives from the device (JSON or raw bytes).
  bool read_control_message(int timeout_ms, std::string* json,
                            std::string* err = nullptr);

  Stats stats() const;
  void reset_stats();

  void set_trace(TraceLevel level, TraceSink sink = TraceSink());
  TraceLevel trace_level() const;

  void on_event(EventCallback cb);

  // Last resolved HID paths / media endpoints (empty when unavailable).
  std::string hidraw_path() const;         // control (IF0)
  std::string video_hidraw_path() const;   // video (IF1)
  std::string media_endpoint() const;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace usbdisplay
