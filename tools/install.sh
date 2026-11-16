#!/usr/bin/env bash
# USB_DISPLAY 一次性安装脚本：sudo 运行一次，之后普通用户即可投屏（无需 sudo/chmod/拔插）
# 用法: sudo tools/install.sh [--uninstall]
set -euo pipefail

RULE_NAME="99-usbdisplay.rules"
RULE_DST="/etc/udev/rules.d/$RULE_NAME"
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
RULE_SRC="$ROOT/tools/$RULE_NAME"

if [[ "${EUID:-$(id -u)}" -ne 0 ]]; then
  echo "请用 sudo 运行：sudo $0" >&2
  exit 1
fi

if [[ "${1:-}" == "--uninstall" ]]; then
  rm -f "$RULE_DST"
  udevadm control --reload || true
  udevadm trigger || true
  echo "[usbdisplay] 已卸载 udev 规则：$RULE_DST"
  exit 0
fi

if [[ ! -f "$RULE_SRC" ]]; then
  echo "找不到规则文件：$RULE_SRC" >&2
  exit 1
fi

install -m 0644 "$RULE_SRC" "$RULE_DST"
udevadm control --reload
# 让当前已插入的设备立即获得权限，并触发 hidraw 规则
udevadm trigger --action=change --subsystem-match=usb --attr-match=idVendor=1de1 --attr-match=idProduct=f201 || true
udevadm trigger --action=change --subsystem-match=hidraw || true

echo "[usbdisplay] 安装完成：$RULE_DST"
echo "[usbdisplay] 之后普通用户即可运行，无需 sudo/chmod；程序会自动复位设备，无需手动拔插。"
echo "  投屏示例："
echo "    cd $ROOT"
echo "    tools/usbdisplay_encode.sh input.mp4 stream.h264 60 4M   # 可选：先把任意视频编码成设备兼容码流"
echo "    ./build/usbdisplay_send --live --h264 stream.h264"
