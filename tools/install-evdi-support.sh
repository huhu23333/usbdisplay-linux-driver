#!/usr/bin/env bash
# 安装/卸载 USB Display 的 evdi 专用虚拟显示支持（需要 sudo）。
#
# 用法：
#   sudo tools/install-evdi-support.sh                  安装 evdi 服务、grabber 检查、组与 Xorg 配置
#   sudo tools/install-evdi-support.sh --no-module-install  不自动安装 evdi 模块
#   sudo tools/install-evdi-support.sh --uninstall      卸载本项目的 unit、脚本、组与自有文件
#
# 本脚本只管理本项目自己的 unit、预创建脚本、grabber 引用、共享帧与 Xorg 配置。
# 不卸载 evdi 模块，不删除设备节点，不触碰机器上已有的其他 evdi 设备。
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
UNIT_NAME="usbdisplay-evdi.service"
UNIT_SRC="$ROOT/tools/usbdisplay-evdi.service"
PRECREATE_SRC="$ROOT/tools/evdi-precreate.sh"
LIBEXEC_DIR="/usr/local/libexec/usbdisplay"
UNIT_LIBEXEC="$LIBEXEC_DIR/$UNIT_NAME"
UNIT_SYSTEM="/etc/systemd/system/$UNIT_NAME"
MARKER="/run/usbdisplay/evdi-card"
LINK="/dev/dri/usbdisplay-evdi"
GRABBER_BIN="/usr/local/bin/usbdisplay_evdi_grabber"
SHM_FILE="/dev/shm/usbdisplay-frames"
SOCKET_FILE="/run/usbdisplay/frames.sock"
CONTROL_FILE="/run/usbdisplay/control.sock"
GROUP_NAME="usbdisplay"
XORG_CONF_SRC="${ROOT}/tools/99-usbdisplay-evdi-xorg.conf"
XORG_CONF_DST="/etc/X11/xorg.conf.d/20-usbdisplay-evdi.conf"

log() { printf '[usbdisplay-evdi-install] %s\n' "$*"; }
warn() { printf '[usbdisplay-evdi-install] 警告：%s\n' "$*" >&2; }
die() { printf '[usbdisplay-evdi-install] 错误：%s\n' "$*" >&2; exit 1; }

usage() {
  cat <<'EOF'
用法：
  sudo tools/install-evdi-support.sh                      安装 evdi 服务、组、grabber 检查与 Xorg 配置
  sudo tools/install-evdi-support.sh --no-module-install  跳过自动安装 evdi 模块
  sudo tools/install-evdi-support.sh --uninstall          卸载本项目的 unit、脚本、组与自有文件

安装不会修改 /etc/modprobe.d，也不会删除或抢占已有 evdi 设备。
grabber 必须已经安装到 /usr/local/bin，且只能通过 usbdisplay-evdi.service 运行。
EOF
}

require_root() {
  if [[ "${EUID:-$(id -u)}" -ne 0 ]]; then
    die "请使用 sudo 运行：sudo $0"
  fi
}

ensure_group() {
  if ! getent group "$GROUP_NAME" >/dev/null 2>&1; then
    groupadd --system "$GROUP_NAME"
    log "已创建系统组 $GROUP_NAME"
  fi
  local user="${SUDO_USER:-}"
  if [[ -n "$user" && "$user" != "root" ]]; then
    if id -nG "$user" 2>/dev/null | tr ' ' '\n' | grep -Fxq "$GROUP_NAME"; then
      log "用户 $user 已在 $GROUP_NAME 组"
    else
      gpasswd -a "$user" "$GROUP_NAME" >/dev/null
      log "已将用户 $user 加入 $GROUP_NAME 组；重新登录后共享内存权限生效"
    fi
  fi
}

remove_group_if_empty() {
  getent group "$GROUP_NAME" >/dev/null 2>&1 || return 0
  local user="${SUDO_USER:-}"
  if [[ -n "$user" && "$user" != "root" ]]; then
    gpasswd -d "$user" "$GROUP_NAME" >/dev/null 2>&1 || true
  fi
  local members
  members="$(getent group "$GROUP_NAME" | cut -d: -f4)"
  if [[ -z "$members" ]]; then
    groupdel "$GROUP_NAME" 2>/dev/null || true
    log "已移除空组 $GROUP_NAME"
  else
    log "组 $GROUP_NAME 仍有其他成员，保留该组"
  fi
}

manual_module_help() {
  cat <<'EOF'
无法自动安装 evdi 模块。请按发行版手动安装后重试：

Ubuntu/Debian：
  sudo apt install dkms linux-headers-$(uname -r) evdi-dkms
Fedora（RPMFusion）：
  sudo dnf install akmod-evdi evdi
Arch：
  sudo pacman -S dkms linux-headers
  通过 AUR 安装 evdi-dkms（例如 yay -S evdi-dkms）
通用源码：
  git clone https://github.com/DisplayLink/evdi.git
  cd evdi && make && sudo make install && sudo depmod -a

Secure Boot 开启时需要注册 DKMS MOK，详见 docs/EVDI_SETUP.md。
检查模块：modinfo evdi && sudo modprobe evdi
EOF
}

module_present() {
  command -v modinfo >/dev/null 2>&1 && modinfo evdi >/dev/null 2>&1
}

install_module_if_needed() {
  local no_module_install="$1"
  if module_present; then
    log "已检测到 evdi 模块"
    return 0
  fi
  if [[ "$no_module_install" == "1" ]]; then
    warn "evdi 模块不存在，已按 --no-module-install 跳过自动安装"
    manual_module_help
    return 1
  fi

  log "未检测到 evdi 模块，尝试通过发行版包管理器安装 evdi-dkms"
  local status=1
  if command -v apt-get >/dev/null 2>&1; then
    apt-get install -y evdi-dkms || status=$?
  elif command -v dnf >/dev/null 2>&1; then
    dnf install -y evdi-dkms || status=$?
  elif command -v pacman >/dev/null 2>&1; then
    pacman -S --needed --noconfirm evdi-dkms || status=$?
  elif command -v zypper >/dev/null 2>&1; then
    zypper --non-interactive install evdi-dkms || status=$?
  else
    status=1
  fi

  if module_present; then
    log "evdi 模块安装成功"
    return 0
  fi
  warn "evdi-dkms 自动安装失败（退出码 $status）"
  manual_module_help
  return 1
}

uninstall() {
  log "停止并移除本项目 grabber 服务"
  if systemctl cat "$UNIT_NAME" >/dev/null 2>&1; then
    if ! systemctl disable --now "$UNIT_NAME"; then
      die "无法停止 $UNIT_NAME；为避免运行中删除 grabber 与共享帧，卸载中止"
    fi
  fi
  rm -f "$UNIT_SYSTEM" "$UNIT_LIBEXEC" "$LIBEXEC_DIR/evdi-precreate.sh"
  rmdir "$LIBEXEC_DIR" 2>/dev/null || true
  rm -f "$XORG_CONF_DST" "$MARKER" "$LINK" "$SHM_FILE" "$SOCKET_FILE" "$CONTROL_FILE"
  rm -f "$GRABBER_BIN"
  rmdir /run/usbdisplay 2>/dev/null || true
  systemctl daemon-reload 2>/dev/null || true
  remove_group_if_empty
  log "已移除本项目 unit、预创建脚本、grabber、共享帧与自有文件"
  log "evdi 模块和已创建的设备节点保留到重启或模块重新加载；其他 evdi 设备未触碰"
}

print_permission_hint() {
  local card="$1"
  local user="${SUDO_USER:-}"
  [[ -n "$card" && -e "$card" ]] || return 0
  log "设备节点："
  ls -l "$card" || true
  if [[ -z "$user" || "$user" == "root" ]]; then
    return 0
  fi
  if id -nG "$user" 2>/dev/null | tr ' ' '\n' | grep -Fxq video; then
    log "用户 $user 已在 video 组"
  else
    log "用户 $user 不在 video 组；可执行 sudo usermod -aG video $user 后重新登录"
  fi
  if command -v getfacl >/dev/null 2>&1 && getfacl -p "$card" 2>/dev/null | grep -Fq "user:$user:"; then
    log "节点已有 $user 的 ACL 权限"
  else
    log "或授予临时 ACL：sudo setfacl -m u:$user:rw $card"
  fi
}

print_grabber_hint() {
  log "grabber：$GRABBER_BIN"
  log "共享帧：$SHM_FILE（grabber 创建，0660，组 $GROUP_NAME）"
  log "grabber 与 $UNIT_NAME 同生命周期，不能单独启动；consumer daemon 只读共享帧"
}

graphical_session_active() {
  if command -v loginctl >/dev/null 2>&1; then
    local id type state sessions
    if ! sessions="$(loginctl list-sessions --no-legend 2>/dev/null)"; then
      return 0
    fi
    while read -r id _; do
      [[ -n "$id" ]] || continue
      if ! type="$(loginctl show-session "$id" -p Type --value 2>/dev/null)"; then
        return 0
      fi
      if ! state="$(loginctl show-session "$id" -p State --value 2>/dev/null)"; then
        return 0
      fi
      if [[ "$state" == "active" && "$type" =~ ^(x11|wayland|mir)$ ]]; then
        return 0
      fi
    done <<< "$sessions"
    return 1
  fi
  # 无法确认时保守视为活动会话，避免在运行中的桌面热插拔 evdi。
  return 0
}

install_service() {
  [[ -f "$UNIT_SRC" ]] || die "缺少 $UNIT_SRC"
  [[ -f "$PRECREATE_SRC" ]] || die "缺少 $PRECREATE_SRC"
  [[ -x "$GRABBER_BIN" ]] || die "缺少 $GRABBER_BIN；请先构建并安装 grabber，或运行 sudo ./install.sh"
  command -v systemctl >/dev/null 2>&1 || die "找不到 systemctl；本安装脚本需要 systemd"
  ensure_group

  install -d -m 0755 "$LIBEXEC_DIR"
  install -m 0755 "$PRECREATE_SRC" "$LIBEXEC_DIR/evdi-precreate.sh"
  install -m 0644 "$UNIT_SRC" "$UNIT_LIBEXEC"
  ln -sfn "$UNIT_LIBEXEC" "$UNIT_SYSTEM"

  if [[ -f "$XORG_CONF_SRC" ]]; then
    install -D -m 0644 "$XORG_CONF_SRC" "$XORG_CONF_DST"
    log "已安装 evdi Xorg 配置：$XORG_CONF_DST"
    log "该配置需要重启图形会话或显示管理器后生效"
  fi

  log "重新加载 systemd 并配置 $UNIT_NAME"
  systemctl daemon-reload
  if graphical_session_active; then
    # 图形会话运行中启动会向 Xorg 热插拔 evdi GPU，可能导致 Xorg/KWin 卡死。
    # 只启用服务，设备在下次启动、显示管理器之前预创建并拉起 grabber。
    if ! systemctl enable "$UNIT_NAME" >/dev/null 2>&1; then
      warn "启用 $UNIT_NAME 失败，请查看 systemctl status $UNIT_NAME"
      exit 1
    fi
    log "图形会话运行中，已启用 $UNIT_NAME 但未立即启动"
    log "请重启系统；服务会在显示管理器之前创建专用 evdi 设备并启动 grabber"
    if systemctl is-active --quiet "$UNIT_NAME"; then
      log "当前已有 grabber 在运行；安装未强制重启，重启系统后应用新 unit/二进制"
    fi
  else
    local was_active=0
    if systemctl is-active --quiet "$UNIT_NAME"; then was_active=1; fi
    if ! systemctl enable --now "$UNIT_NAME"; then
      warn "服务启动失败，请查看 journalctl -u $UNIT_NAME 与 docs/EVDI_SETUP.md"
      systemctl status --no-pager --lines=20 "$UNIT_NAME" 2>/dev/null || true
      exit 1
    fi
    if [[ "$was_active" == "1" ]]; then
      systemctl restart "$UNIT_NAME" 2>/dev/null || true
    fi
  fi

  local card="" connector=""
  if [[ -f "$MARKER" ]]; then
    card="$(awk -F= '$1 == "card" { print $2; exit }' "$MARKER")"
    connector="$(awk -F= '$1 == "connector" { print $2; exit }' "$MARKER")"
    log "专用设备 marker：$MARKER"
    sed 's/^/  /' "$MARKER"
    log "分配的 card：${card:-未知}，连接器：${connector:-未知}（固定 1920x1080）"
    if [[ -L "$LINK" ]]; then
      log "稳定链接：$LINK -> $(readlink "$LINK" 2>/dev/null || true)"
    fi
  else
    if graphical_session_active; then
      log "图形会话运行中尚未启动服务，marker 会在重启后由 ExecStartPre 创建"
    else
      warn "服务已启动但未找到 marker；请检查 journalctl -u $UNIT_NAME"
    fi
  fi
  print_permission_hint "$card"
  print_grabber_hint
  log "后续：consumer daemon 只读 $SHM_FILE；X11/Wayland 配置见 docs/EVDI_SETUP.md"
  log "evdi 路径不使用音频；已有 evdi 设备未被修改"
}

main() {
  local uninstall_mode=0
  local no_module_install=0
  while [[ $# -gt 0 ]]; do
    case "$1" in
      --uninstall) uninstall_mode=1 ;;
      --no-module-install) no_module_install=1 ;;
      --help|-h) usage; exit 0 ;;
      *) printf '未知参数：%s\n' "$1" >&2; usage >&2; exit 2 ;;
    esac
    shift
  done

  require_root
  if [[ "$uninstall_mode" == "1" ]]; then
    uninstall
    return 0
  fi
  if ! install_module_if_needed "$no_module_install"; then
    die "evdi 模块不可用，安装中止"
  fi
  install_service
}

main "$@"
