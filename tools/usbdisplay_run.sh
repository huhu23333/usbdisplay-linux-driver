#!/usr/bin/env bash
# 普通用户运行：自动设置 Intel iHD 环境（若存在本地编译驱动）并投屏
# 用法: tools/usbdisplay_run.sh <stream.h264> [usbdisplay_send 额外参数...]
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
if [[ $# -lt 1 ]]; then echo "usage: $0 <stream.h264> [args...]" >&2; exit 2; fi
STREAM="$1"; shift || true
for VA in "$ROOT/work/va2" "$ROOT/../work/va2" "$ROOT/../../work/va2"; do
  if [[ -d "$VA/lib" ]]; then
    export LD_LIBRARY_PATH="$VA/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
    export LIBVA_DRIVERS_PATH="$VA/lib/dri"
    export LIBVA_DRIVER_NAME=iHD
    break
  fi
done
exec "$ROOT/build/usbdisplay_send" --live --h264 "$STREAM" "$@"
