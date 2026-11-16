# usbdisplay - Linux userspace sender for Actions Micro "USB Display" (EZMira)

C++17 library + CLI tools that push caller-supplied H.264 access units and audio
to an Actions Micro USB Display receiver (VID_1DE1&PID_F201, AM8268N-class) from
unprivileged Linux userspace. No kernel modules, no system file modifications,
no screen capture.

The protocol in this README is the **hardware/Windows-capture verified** one
(USB_PROTOCOL_VERIFIED.md, 1.pcapng, work/hw/live_fixed.py and live_seq.py).
Lead validated end-to-end display on the real receiver with a compatible stream;
the previous reverse-engineering guesses are marked deprecated at the end.

## Status (verified protocol revision)

| Area | State |
|---|---|
| Build (cmake + g++ 11, -Wall -Wextra -Wpedantic) | **clean, 0 warnings** |
| Tests (ctest) | **14/14 pass** |
| Control channel JSON-RPC over hidraw IF0 (heartbeat with session-id, events) | hardware verified |
| Verified 8-step startup handshake + isAllowed wait | implemented, matches capture |
| Video: libusb EP2 OUT, 4096-byte URBs, first-URB length/width/height header | implemented, offline validated |
| Audio: ALSA plughw:USBDISPLAY,0 48 kHz stereo S16 | hardware verified |
| Annex-B AU start-code preservation | unit tested byte-exact |
| Frame-rate derivation from SPS VUI + absolute-time pacing | implemented, unit tested |
| Offline fake-device URB validation | integration tested |
| Automatic reset/re-enumeration session recovery (--auto-reset default on) | implemented, retry state machine unit tested |

## Build

Requirements (present on the target machine): cmake >= 3.16, g++ >= 11,
libusb-1.0 + pkg-config, libudev headers, ALSA dev headers (optional; audio is a
no-op without them). No hidapi needed: control uses /dev/hidraw directly.

```
cd /media/huhu233/Data2/projects2/DSH/reverse
cmake -S impl -B build
cmake --build build -j
```

Artifacts in build/: libusbdisplay.a, usbdisplay_diag, usbdisplay_send,
usbdisplay_probe, usbdisplay_json, usbdisplay_audio, fake_device, tests.

## Tests

```
ctest --test-dir build --output-on-failure
```

14 tests: JSON, utils, Annex-B AU splitter (including a byte-exact start-code
preservation test), legacy IP wire codec, packetizer, WAV, HID control report
framing, verified 4096-byte URB framing, H.264 SPS/VUI frame-rate parsing, the
auto-reset retry state machine, and four cross-process integration tests
(fake_device validates what usbdisplay_send produced: legacy TCP/UDP/RTP +
verified USB URB dump).

Offline verified-protocol loop (no hardware; does not touch the receiver):

```
./build/usbdisplay_send --synthetic-frames 6 --dump-reports /tmp/urbs.bin
./build/fake_device --usb-dump /tmp/urbs.bin --expect-frames 6 --hex-bytes 64
# observed:
#   sending: ... video=1920x1080@60.000 fps (SPS VUI) ... urb=4096B/id2
#   USB dump: 49 URBs, urb_size=4096, video_frames=6, aud_missing=0
#   RESULT: PASS
```

Fake-device check details: every message must start with a report id 0x02 URB
carrying BE32 frame length and BE32 width/height, continuation URBs must have
zeros in bytes 1..15, every frame must still start with an Annex-B start code and
begin with an AUD (aud_missing must be 0).

## Tools

### usbdisplay_send - verified sender

Default is a safe offline dry run. Live mode writes the control HID reports and
the libusb EP2 video URBs:

```
# real device (Lead runs these; stream must come from a compatible encoder)
./build/usbdisplay_send --live --h264 stream.h264
./build/usbdisplay_send --live --h264 stream.h264 --width 1920 --height 1080
./build/usbdisplay_send --live --synthetic-frames 60     # framing smoke test
# offline
./build/usbdisplay_send --h264 stream.h264 --dump-reports /tmp/urbs.bin
./build/usbdisplay_send --synthetic-frames 6 --dump-reports /tmp/urbs.bin
```

Behaviour:
* Frame rate comes from the SPS VUI (num_units_in_tick/time_scale) when --fps is
  not given; otherwise --fps; otherwise 60 fps. Pacing uses an absolute
  CLOCK_MONOTONIC base (no accumulated sleep drift).
* Default is a single sequential pass (aus[0], aus[1], ...). --repeat N replays
  N passes; --loop restarts the session each pass. Both require the first AU to
  be an IDR frame (otherwise the sender refuses, because frame-number/decoder
  state would break).
* --inject-aud prepends a default AUD to access units lacking one; input start
  codes are always preserved verbatim and validated before sending.
* --usb-transfer auto|interrupt|bulk selects the libusb transfer type (auto
  tries interrupt then bulk; the Windows reference used a bulk write on EP2).
* --handshake is on by default in live mode (--no-handshake disables).
* --auto-reset is on by default: if startup/handshake fails (no isAllowed, no
  app-launched reply, claim busy) the device is reset and re-enumerated and the
  full handshake is retried (up to --reset-retries 3 times, --reset-wait-ms
  5000 per reset). --no-auto-reset disables this. --reset-on-exit additionally
  resets on clean shutdown (default off).
* --no-pace exists for offline framing tests only.
* Audio: --wav FILE plays through the device USB audio interface via ALSA.

### usbdisplay_diag - discovery and probes

```
./build/usbdisplay_diag --offline     # codec self-test, no hardware
./build/usbdisplay_diag --probe-json  # session-id heartbeat + device replies
./build/usbdisplay_diag               # USB/HID/ALSA/network report
```

It maps hidraw nodes by USB interface number (IF0 control), reports descriptor
report sizes, checks whether this process may open the USB device for libusb
(printing a chmod/udev hint if not), lists the ALSA USBDISPLAY card, and can send
a non-destructive heartbeat with a numeric session-id.

### usbdisplay_json - JSON-RPC control session

```
./build/usbdisplay_json --seconds 10
./build/usbdisplay_json --send '{"jsonrpc":"2.0","method":"app-launched","params":{...}}'
./build/usbdisplay_json --stdin --heartbeat-ms 1500
```

Sends heartbeat with a numeric session-id every heartbeat interval, prints
device documents, and auto-answers device requests that carry an id
(projection-request-with-tx -> {"result":{"action":"allow"}}).

### usbdisplay_probe - read device HID input reports (read-only by default)

```
./build/usbdisplay_probe --seconds 5 --hex 64
```

### usbdisplay_audio - standalone ALSA playback

```
./build/usbdisplay_audio --tone-hz 440 --seconds 2
./build/usbdisplay_audio --wav music.wav
```

### fake_device - simulated receiver / framing validator

```
./build/fake_device --usb-dump /tmp/urbs.bin --expect-frames 6 --hex-bytes 64
./build/fake_device --tcp-port 9000 --expect-frames 30     # legacy IP profile
./build/fake_device --udp-port 9000 --wire rtp
```

Exit code 0 only when all framing/AU/AUD checks pass (RESULT: PASS).

## Verified protocol (implemented here)

### Control channel - IF0, /dev/hidraw, HID report id 1, exactly 4096 bytes

```
off  size field
0x00 1    report id = 0x01
0x01 4    u32 BE JSON byte length
0x05 11   zero
0x10 N    UTF-8 JSON-RPC 2.0 document
...       zero padding to 4096
```

Device -> host uses the same header; the device JSON is pretty-printed
(tabs/CRLF) and must be parsed leniently. JSON larger than 4080 bytes is split
over consecutive reports, each repeating the same total length.

Startup sequence (strict order, wait for isAllowed before video):

```
heartbeat with session-id                   (session-id is a numeric string)
{"id":1,"jsonrpc":"2.0","method":"app-launched",
 "params":{"platform":"windows","platform-machine-name":"<host>",
           "product-type":"QuattroTx CT10","res-height":1080,"res-width":1920}}
{"jsonrpc":"2.0","method":"app_tx_mouse_icon_change",
 "params":{"cursor_base64_data":""}}
{"jsonrpc":"2.0","method":"notify-projection-stopped"}
{"jsonrpc":"2.0","method":"common.data_passthrough",
 "params":{"data":"AQ==","id":103,"size":1}}
{"jsonrpc":"2.0","method":"notify-projection-stopped"}
{"id":3,... app-launched ...}              (same params)
{"id":4,"jsonrpc":"2.0","method":"notify-projection-started"}
  -> device replies {"result":"isAllowed"}  then start sending video
```

Heartbeat afterwards every ~1.2 s:
{"jsonrpc":"2.0", "method": "heartbeat", "params": {"session-id": "<numeric>"}}
The device echoes the same session-id and increments jrpcSeq, and pushes
notify-app-accessories-are-ready / tx-data-updated.

Device events handled: notify-app-resend-stream-iframe-metadata-info raises a
KeyframeRequested event; notify-streaming-codec-need-change is logged; device
requests with an id are answered (projection requests get action allow).

### Video channel - IF1, EP2 OUT via libusb, exactly 4096-byte URBs

**The HID descriptor advertises 12288-byte reports, but the verified app writes
4096-byte URBs.** Use libusb: detach the kernel driver from interface 1, claim
it, find the OUT endpoint (0x02, interrupt/high-bandwidth), and write 4096-byte
URBs.

```
first URB of one frame:
  [0]      = 0x02
  [1..4]   = u32 BE  L = total H.264 frame length (INCLUDING start codes)
  [5..7]   = 0x00 x3
  [8..11]  = u32 BE width  (e.g. 1920)
  [12..15] = u32 BE height (e.g. 1080)
  [16..]   = first 4080 bytes of the Annex-B access unit, zero padded to 4096

continuation URB (when L > 4080):
  [0]      = 0x02
  [1..15]  = 0x00 x15
  [16..]   = next 4080 bytes, zero padded to 4096
```

Each video message is exactly one Annex-B H.264 access unit beginning with an
AUD (00 00 00 01 09 10/30), typically AUD + SPS + PPS + SEI + IDR for the first
frame and AUD + PPS + SEI + slice afterwards. Access units must keep their
original 00 00 00 01 start codes; the sender validates this and refuses to drop
them (that was the root cause of the earlier black screen).

Scheduling: use the real stream frame rate (SPS VUI num_units_in_tick /
time_scale, or --fps) on an absolute monotonic clock. Do not hardcode 60 fps or
loop with aus[i % len]: frame-number/reference state would regress. The default
is one sequential pass; looping restarts the session and requires the first AU to
be IDR.

### Audio

USB Audio class only: ALSA playback on plughw:USBDISPLAY,0, 2 ch, S16_LE,
48 kHz. No custom HID audio packets.

## Automatic device reset / session restart

The receiver firmware only accepts a session after a fresh enumeration. Instead
of asking the operator to replug, the sender can reset the USB device itself
(verified in work/hw/reset_test.py):

1. On startup, or when the handshake fails / the device stops answering
   (isAllowed timeout, no app-launched reply, claim busy), all hidraw and
   libusb handles are closed.
2. libusb_reset_device() is called on 1de1:f201.
3. The sender polls for the device to re-enumerate (new USB address, up to
   auto_reset_wait_ms = 5 s) and waits for the IF0 hidraw node to reappear by
   sysfs VID/PID + interface number (never a hardcoded hidraw name).
4. The full verified 8-step handshake is re-run; the reset+retry cycle is
   repeated up to 3 times, logging "auto-reset: attempt N (previous attempt
   failed: ...)".
5. A clean exit does not reset by default. The next run simply auto-resets if
   its first handshake fails, so two consecutive runs need no replug. Use
   --reset-on-exit to also reset on shutdown, --no-auto-reset to disable the
   recovery entirely.

Because the reset removes and re-adds the kernel HID nodes, IF1 is detached from
hid-generic while streaming and re-attached on clean shutdown; after a crash a
replug (or the next auto-reset run) restores it.

## Permissions

One-time setup (creates persistent access to both the USB device node needed by
libusb and the IF0 hidraw node):

```
sudo cp tools/99-usbdisplay.rules /etc/udev/rules.d/
sudo udevadm control --reload
sudo udevadm trigger
```

The rule file lives in the repository at tools/99-usbdisplay.rules (the project
never installs it automatically). If a device open still fails before the rule is
installed, the tools print this exact command; a temporary fallback for the
current boot is "sudo chmod 666 /dev/bus/usb/BBB/DDD" (bus/device numbers change
after every reset/replug).
* Detaching interface 1 from the kernel HID driver removes /dev/hidraw5 while
  the sender is active; the sender re-attaches it on clean shutdown. After a
  crash, replugging the device restores it.

## Deprecated (pre-verified guesses; do not use)

* 12288-byte HID report video framing on /dev/hidraw5, report-id chunking.
* --frame-mode app-inner / ugreen and the 32/36-byte inner header, RRIM/TADV
  magic, p3/p4 parametrisation.
* common.assign_role / common.request_stream / common.get_device_description
  handshake (this firmware answers "Method not found" for several of these).
* hardcoded 60 fps, aus[i % len] looping, per-report headers for continuation
  URBs.
* Legacy TCP/UDP "Wi-Fi profile" code remains buildable and offline-tested, but
  the USB device does not use it.

## Known limitations

* Encoder compatibility: this firmware is picky about the H.264 slice data. The
  protocol above is correct and was validated with a Windows-captured/compatible
  stream; arbitrary x264 output may still show black (see
  USB_PROTOCOL_VERIFIED.md section 4). The sender is transport-correct and
  passthrough-friendly; producing a compatible bitstream is the caller's job.
* No HEVC, no cursor/back-channel input injection, no Wi-Fi discovery, no OTA.
* The high-bandwidth interrupt endpoint is written through libusb with explicit
  detach/claim; root or udev permissions are required.

## Layout

```
impl/include/usbdisplay/   public API (types.h, device.h, protocol.h, version.h)
impl/src/transport/        libusb EP2 video sink, hidraw control, libusb probe
impl/src/hid/              HID control report framing + URB/report dumps
impl/src/control/          JSON-RPC + hidraw control channel
impl/src/proto/            Annex-B splitter, H.264 SPS/VUI parser, WAV, IP codecs
impl/src/audio/            ALSA player
impl/src/common/           logging, minimal JSON, utilities
impl/tools/                diag, send, probe, json session, audio, fake device
impl/tests/                unit tests + cross-process integration scripts
```
