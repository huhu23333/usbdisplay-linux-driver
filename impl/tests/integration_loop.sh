#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# 跨进程封装测试。
#   用法：integration_loop.sh <构建目录> tcp|udp|rtp|hid
set -u

BIN=${1:?usage: integration_loop.sh <build-dir> tcp|udp|rtp|hid}
MODE=${2:-tcp}
TMP="$(mktemp -d)"
PORT=$(( 35000 + RANDOM % 20000 ))
REPORT="$TMP/report.txt"
SEND_RC=0
FAKE_RC=0

cleanup() {
  if [ -n "${FPID:-}" ] && kill -0 "$FPID" 2>/dev/null; then
    kill "$FPID" 2>/dev/null
    wait "$FPID" 2>/dev/null
  fi
  rm -rf "$TMP"
}
trap cleanup EXIT

if [ "$MODE" = "hid" ]; then
  DUMP="$TMP/urbs.bin"
  "$BIN/usbdisplay_send" --synthetic-frames 6 --no-pace \
      --dump-reports "$DUMP" --trace warn >"$TMP/send.log" 2>&1
  SEND_RC=$?
  "$BIN/fake_device" --usb-dump "$DUMP" --expect-frames 6 \
      --report "$REPORT" --hex-bytes 64 >"$TMP/fake.log" 2>&1
  FAKE_RC=$?
  cat "$TMP/send.log" "$TMP/fake.log"
  [ -f "$REPORT" ] && cat "$REPORT"
  echo "sender rc=$SEND_RC fake rc=$FAKE_RC"
  if [ "$SEND_RC" -ne 0 ] || [ "$FAKE_RC" -ne 0 ]; then
    echo "INTEGRATION FAIL ($MODE)"
    exit 1
  fi
  if ! grep -q "RESULT: PASS" "$REPORT"; then
    echo "INTEGRATION FAIL: no PASS in report ($MODE)"
    exit 1
  fi
  echo "INTEGRATION PASS ($MODE)"
  exit 0
fi

case "$MODE" in
  tcp)
    FAKE_ARGS="--tcp-port $PORT"
    SEND_ARGS="--sink tcp --host 127.0.0.1 --port $PORT"
    ;;
  udp)
    FAKE_ARGS="--udp-port $PORT"
    SEND_ARGS="--sink udp --host 127.0.0.1 --port $PORT --audio-port $((PORT+1))"
    ;;
  rtp)
    FAKE_ARGS="--udp-port $PORT --wire rtp"
    SEND_ARGS="--sink udp --wire rtp --host 127.0.0.1 --port $PORT --audio-port $((PORT+1))"
    ;;
  *)
    echo "unknown mode $MODE" >&2
    exit 2
    ;;
esac

"$BIN/fake_device" $FAKE_ARGS --expect-frames 6 --max-seconds 20 \
    --report "$REPORT" --hex-bytes 64 >"$TMP/fake.log" 2>&1 &
FPID=$!

for _ in $(seq 1 200); do
  grep -q LISTENING "$TMP/fake.log" 2>/dev/null && break
  sleep 0.05
done
if ! grep -q LISTENING "$TMP/fake.log"; then
  echo "fake_device did not start:" >&2
  cat "$TMP/fake.log" >&2
  exit 1
fi

"$BIN/usbdisplay_send" --synthetic-frames 6 --fps 200 --no-pace --no-control \
    $SEND_ARGS --trace warn >"$TMP/send.log" 2>&1
SEND_RC=$?

for _ in $(seq 1 100); do
  kill -0 "$FPID" 2>/dev/null || break
  sleep 0.1
done
if kill -0 "$FPID" 2>/dev/null; then
  echo "fake_device did not finish, killing" >&2
  kill "$FPID" 2>/dev/null
fi
wait "$FPID" 2>/dev/null
FAKE_RC=$?

cat "$TMP/fake.log"
[ -f "$REPORT" ] && cat "$REPORT"
echo "sender rc=$SEND_RC fake rc=$FAKE_RC"
if [ "$SEND_RC" -ne 0 ] || [ "$FAKE_RC" -ne 0 ]; then
  echo "INTEGRATION FAIL ($MODE)"
  exit 1
fi
if ! grep -q "RESULT: PASS" "$REPORT"; then
  echo "INTEGRATION FAIL: no PASS in report ($MODE)"
  exit 1
fi
echo "INTEGRATION PASS ($MODE)"
