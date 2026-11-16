#!/usr/bin/env bash
# 视频设备规则安装/卸载：USB 与 HID 访问权限。
# 用法：sudo tools/install-video-rules.sh          安装
#       sudo tools/install-video-rules.sh --uninstall 卸载
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
SRC="$ROOT/tools/99-usbdisplay-usb.rules"
DST="/etc/udev/rules.d/99-usbdisplay-usb.rules"
if [[ "${EUID:-$(id -u)}" -ne 0 ]]; then echo "请用 sudo 运行：sudo $0" >&2; exit 1; fi
case "${1:-}" in
  --uninstall)
    rm -f "$DST"
    udevadm control --reload || true
    udevadm trigger --action=change --subsystem-match=usb --attr-match=idVendor=1de1 --attr-match=idProduct=f201 || true
    udevadm trigger --action=change --subsystem-match=hidraw || true
    echo "[usbdisplay] 已卸载视频设备规则（USB/HID 权限）"
    ;;
  "")
    install -m 0644 "$SRC" "$DST"
    udevadm control --reload
    udevadm trigger --action=change --subsystem-match=usb --attr-match=idVendor=1de1 --attr-match=idProduct=f201 || true
    udevadm trigger --action=change --subsystem-match=hidraw || true
    echo "[usbdisplay] 已安装视频设备规则（USB/HID 权限），建议重新插拔设备"
    ;;
  *) echo "未知参数：$1"; echo "用法：sudo $0 [--uninstall]" >&2; exit 2;;
esac
