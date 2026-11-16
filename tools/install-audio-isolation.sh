#!/usr/bin/env bash
# 音频隔离规则安装/卸载：PulseAudio 忽略该 USB 声卡。
# 未安装或卸载后，设备作为普通桌面音频输出设备使用。
# 用法：sudo tools/install-audio-isolation.sh          安装音频隔离
#       sudo tools/install-audio-isolation.sh --uninstall 卸载音频隔离
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
SRC="$ROOT/tools/99-usbdisplay-audio-ignore.rules"
DST="/etc/udev/rules.d/99-usbdisplay-audio-ignore.rules"
if [[ "${EUID:-$(id -u)}" -ne 0 ]]; then echo "请用 sudo 运行：sudo $0" >&2; exit 1; fi
case "${1:-}" in
  --uninstall)
    rm -f "$DST"
    udevadm control --reload || true
    udevadm trigger --action=change --subsystem-match=sound || true
    echo "[usbdisplay] 已卸载音频隔离规则；设备恢复为普通桌面音频输出设备"
    ;;
  "")
    install -m 0644 "$SRC" "$DST"
    udevadm control --reload
    udevadm trigger --action=change --subsystem-match=sound || true
    echo "[usbdisplay] 已安装音频隔离规则；PulseAudio 不再显示该声卡，程序音频单独送到设备"
    echo "[usbdisplay] 建议重新插拔设备使规则生效"
    ;;
  *) echo "未知参数：$1"; echo "用法：sudo $0 [--uninstall]" >&2; exit 2;;
esac
