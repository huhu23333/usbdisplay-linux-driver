#!/usr/bin/env bash
# 把任意音视频输入编码成 USB_DISPLAY 设备兼容的 H.264 码流
# 用法: tools/usbdisplay_encode.sh <input> <output.h264> [fps=60] [bitrate=4M] [--vaapi]
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
if [[ $# -lt 2 ]]; then
  echo "usage: $0 <input> <output.h264> [fps=60] [bitrate=4M] [--vaapi]" >&2
  exit 2
fi
IN="$1"; OUT="$2"; FPS="${3:-60}"; BR="${4:-4M}"; MODE="${5:-}"
COMMON=(-profile:v main -level:v 4.2 -b:v "$BR" -maxrate "$BR" -pix_fmt yuv420p -r "$FPS")
X264=(-x264-params "aud=1:repeat-headers=1:bframes=0:ref=1:keyint=120:scenecut=0")
if [[ "$MODE" == "--vaapi" ]]; then
  for VA in "$ROOT/work/va2" "$ROOT/../work/va2" "$ROOT/../../work/va2"; do
    if [[ -d "$VA/lib" ]]; then
      export LD_LIBRARY_PATH="$VA/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
      export LIBVA_DRIVERS_PATH="$VA/lib/dri"
      export LIBVA_DRIVER_NAME=iHD
      break
    fi
  done
  exec ffmpeg -y -hide_banner -vaapi_device /dev/dri/renderD128 -i "$IN" \
    -vf "format=nv12,hwupload" -c:v h264_vaapi "${COMMON[@]}" -bf 0 -refs 1 -f h264 "$OUT"
else
  exec ffmpeg -y -hide_banner -i "$IN" -c:v libx264 "${COMMON[@]}" "${X264[@]}" -f h264 "$OUT"
fi
