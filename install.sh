#!/usr/bin/env bash
# 一键安装 USB Display：视频权限规则、evdi 扩展屏支持、daemon 与用户服务。
#
# 用法：
#   sudo ./install.sh              安装
#   sudo ./install.sh --uninstall  卸载（会同时移除手动视频输出所需规则）
#   sudo ./install.sh --uninstall --yes  跳过卸载确认
#
# 本脚本不安装音频隔离规则，不修改音频配置。evdi 扩展显示屏是可选的，
# 手动 usbdisplay_send 视频输出路径仍然可用。
set -euo pipefail

ROOT="$(cd "$(dirname "$0")" && pwd)"
DAEMON_BIN="/usr/local/bin/usbdisplay_daemon"
GRABBER_BIN="/usr/local/bin/usbdisplay_evdi_grabber"
CTL_BIN="/usr/local/bin/usbdisplay_evdi_ctl"
DAEMON_SERVICE="usbdisplay-daemon.service"
DAEMON_UNIT_SRC="$ROOT/tools/$DAEMON_SERVICE"

log() { printf '[usbdisplay-install] %s\n' "$*"; }
warn() { printf '[usbdisplay-install] 警告：%s\n' "$*" >&2; }
die() { printf '[usbdisplay-install] 错误：%s\n' "$*" >&2; exit 1; }

usage() {
  cat <<'EOF'
用法：
  sudo ./install.sh                   安装视频规则、evdi 支持、daemon 与用户服务
  sudo ./install.sh --uninstall       卸载本项目安装内容
  sudo ./install.sh --uninstall --yes 跳过卸载确认

不安装音频隔离规则；evdi 与手动视频输出解耦，扩展显示屏为可选功能。
EOF
}

require_root() {
  if [[ "${EUID:-$(id -u)}" -ne 0 ]]; then
    die "请使用 sudo 运行：sudo $0"
  fi
}

user_name() {
  local user="${SUDO_USER:-}"
  if [[ -z "$user" || "$user" == "root" ]]; then
    die "请以普通用户身份通过 sudo 运行；需要 SUDO_USER 来安装 systemd user 服务"
  fi
  printf '%s\n' "$user"
}

user_home() {
  local user="$1"
  local home
  home="$(getent passwd "$user" | cut -d: -f6)"
  [[ -n "$home" ]] || die "无法确定用户 $user 的家目录"
  printf '%s\n' "$home"
}

user_group() {
  id -gn "$1"
}

user_runtime_dir() {
  printf '/run/user/%s\n' "$(id -u "$1")"
}

check_systemd() {
  command -v systemctl >/dev/null 2>&1 || die "未找到 systemctl；本脚本依赖 systemd 用户服务"
}

ensure_build_dependencies() {
  local missing=0
  command -v cmake >/dev/null 2>&1 || missing=1
  command -v pkg-config >/dev/null 2>&1 || missing=1
  command -v c++ >/dev/null 2>&1 || missing=1
  if command -v pkg-config >/dev/null 2>&1; then
    pkg-config --exists x264 || missing=1
    pkg-config --exists libswscale || missing=1
  fi
  local ld_cache
  ld_cache="$(ldconfig -p 2>/dev/null || true)"
  if ! grep -q 'libevdi' <<<"$ld_cache"; then
    missing=1
  fi
  if [[ "$missing" -eq 0 ]]; then
    return 0
  fi

  log "检测到缺少 daemon 构建依赖，尝试通过发行版包管理器安装"
  local status=0
  if command -v apt-get >/dev/null 2>&1; then
    apt-get install -y cmake g++ pkg-config libx264-dev libswscale-dev libevdi0 || status=$?
  elif command -v dnf >/dev/null 2>&1; then
    dnf install -y cmake gcc-c++ pkgconf-pkg-config x264-devel ffmpeg-devel evdi || status=$?
  elif command -v pacman >/dev/null 2>&1; then
    pacman -S --needed --noconfirm cmake gcc pkgconf x264 ffmpeg evdi || status=$?
  elif command -v zypper >/dev/null 2>&1; then
    zypper --non-interactive install cmake gcc-c++ pkg-config libx264-devel \
      libswscale-devel evdi || status=$?
  fi

  local final_cache
  final_cache="$(ldconfig -p 2>/dev/null || true)"
  if [[ "$status" -ne 0 ]] || ! command -v cmake >/dev/null 2>&1 ||
      ! command -v pkg-config >/dev/null 2>&1 ||
      ! pkg-config --exists x264 2>/dev/null ||
      ! pkg-config --exists libswscale 2>/dev/null ||
      ! grep -q 'libevdi' <<<"$final_cache"; then
    die "daemon 构建依赖仍不完整；请参考 docs/EVDI_SETUP.md 手动安装 cmake、C++ 编译器、pkg-config、libx264、libswscale 与 libevdi"
  fi
  log "构建依赖已就绪"
}

build_binaries_if_needed() {
  local user="$1"
  local home="$2"
  if [[ -x "$ROOT/build/usbdisplay_daemon" && -x "$ROOT/build/usbdisplay_evdi_grabber" ]]; then
    return 0
  fi
  log "build 中缺少 daemon 或 grabber，按 $user 身份构建"
  command -v cmake >/dev/null 2>&1 || die "未找到 cmake，无法构建"
  sudo -u "$user" env HOME="$home" cmake -S "$ROOT/impl" -B "$ROOT/build"
  sudo -u "$user" env HOME="$home" cmake --build "$ROOT/build" \
    --target usbdisplay_evdi_grabber usbdisplay_daemon usbdisplay_evdi_ctl -j"$(nproc)"
  [[ -x "$ROOT/build/usbdisplay_evdi_grabber" ]] ||
    die "grabber 构建失败；请检查 libevdi 与 docs/EVDI_SETUP.md"
  [[ -x "$ROOT/build/usbdisplay_daemon" ]] ||
    die "daemon 构建失败；请检查 libx264、libswscale 与 docs/EVDI_SETUP.md"
}

atomic_install_binary() {
  local source="$1"
  local destination="$2"
  local temp
  temp="$(dirname "$destination")/.usbdisplay-install.$$.tmp"
  install -m 0755 "$source" "$temp"
  mv -f "$temp" "$destination"
}

install_user_service() {
  local user="$1"
  local home="$2"
  local unit_dir="$home/.config/systemd/user"
  local unit_path="$unit_dir/$DAEMON_SERVICE"
  local runtime
  runtime="$(user_runtime_dir "$user")"

  # 以目标用户身份创建目录，避免 root 新建 .config 后影响用户后续配置。
  sudo -u "$user" env HOME="$home" mkdir -p "$unit_dir"
  local unit_tmp="$unit_dir/.$DAEMON_SERVICE.$$.tmp"
  install -m 0644 "$DAEMON_UNIT_SRC" "$unit_tmp"
  chown "$user:$(user_group "$user")" "$unit_tmp"
  mv -f "$unit_tmp" "$unit_path"

  if [[ -d "$runtime" ]]; then
    if sudo -u "$user" env HOME="$home" XDG_RUNTIME_DIR="$runtime" \
        systemctl --user daemon-reload; then
      if sudo -u "$user" env HOME="$home" XDG_RUNTIME_DIR="$runtime" \
          systemctl --user enable "$DAEMON_SERVICE"; then
        log "用户服务已启用（未提前启动）：$DAEMON_SERVICE"
        printf '登录桌面后启动：systemctl --user start %s\n' "$DAEMON_SERVICE"
        return 0
      fi
    fi
  fi
  warn "当前用户会话不可用，无法启用 user 服务"
  printf '请登录桌面后执行：\n  systemctl --user daemon-reload\n  systemctl --user enable %s\n  systemctl --user start %s\n' "$DAEMON_SERVICE" "$DAEMON_SERVICE"
}

uninstall_user_service() {
  local user="$1"
  local home="$2"
  local unit_path="$home/.config/systemd/user/$DAEMON_SERVICE"
  local runtime
  runtime="$(user_runtime_dir "$user")"
  if [[ -d "$runtime" ]]; then
    sudo -u "$user" env HOME="$home" XDG_RUNTIME_DIR="$runtime" \
      systemctl --user disable --now "$DAEMON_SERVICE" 2>/dev/null || true
    sudo -u "$user" env HOME="$home" XDG_RUNTIME_DIR="$runtime" \
      systemctl --user daemon-reload 2>/dev/null || true
  fi
  rm -f "$unit_path"
}

stop_evdi_unit_or_die() {
  if systemctl cat usbdisplay-evdi.service >/dev/null 2>&1; then
    if ! systemctl disable --now usbdisplay-evdi.service; then
      die "无法停止 usbdisplay-evdi.service；为避免在运行中删除 grabber，卸载中止"
    fi
  fi
}

remove_legacy_combined_rule() {
  local legacy="/etc/udev/rules.d/99-usbdisplay.rules"
  if [[ -f "$legacy" ]]; then
    warn "检测到旧版合并规则 $legacy，将移除以免与拆分规则重复或意外启用音频隔离"
    rm -f "$legacy"
    udevadm control --reload 2>/dev/null || true
    udevadm trigger --action=change --subsystem-match=usb \
      --attr-match=idVendor=1de1 --attr-match=idProduct=f201 2>/dev/null || true
    udevadm trigger --action=change --subsystem-match=hidraw 2>/dev/null || true
    udevadm trigger --action=change --subsystem-match=sound 2>/dev/null || true
  fi
}

install_all() {
  local user="$1"
  local home="$2"
  if [[ -r /etc/os-release ]]; then
    # shellcheck disable=SC1091
    . /etc/os-release
    if [[ "${ID:-}" != "debian" && "${ID:-}" != "ubuntu" ]]; then
      log "检测到发行版 ${PRETTY_NAME:-未知}；evdi 安装脚本会按 apt/dnf/pacman/zypper 处理，视频权限规则使用 udev"
    fi
  fi
  remove_legacy_combined_rule
  log "安装 USB/HID 视频权限规则"
  "$ROOT/tools/install-video-rules.sh"

  ensure_build_dependencies
  build_binaries_if_needed "$user" "$home"
  atomic_install_binary "$ROOT/build/usbdisplay_evdi_grabber" "$GRABBER_BIN"
  atomic_install_binary "$ROOT/build/usbdisplay_daemon" "$DAEMON_BIN"
  atomic_install_binary "$ROOT/build/usbdisplay_evdi_ctl" "$CTL_BIN"
  log "grabber 已安装到 $GRABBER_BIN"
  log "daemon 已安装到 $DAEMON_BIN"
  log "控制 CLI 已安装到 $CTL_BIN"

  log "安装 evdi 扩展显示屏支持（unit 内先预创建再启动 grabber）"
  "$ROOT/tools/install-evdi-support.sh"

  install_user_service "$user" "$home"
  log "不安装音频隔离规则；evdi 路径不使用音频"
  log "安装完成。手动视频输出仍可使用 usbdisplay_send；扩展显示屏由 $DAEMON_SERVICE 提供"
  log "Wayland/KWin 多 GPU 配置见 docs/EVDI_SETUP.md"
}

uninstall_all() {
  local user="$1"
  local home="$2"
  cat <<'EOF'
卸载会移除：
  - 当前用户的 usbdisplay-daemon user 服务与 /usr/local/bin/usbdisplay_daemon
  - evdi 预创建服务与 marker（evdi 模块与设备节点保留）
  - USB/HID 视频权限规则，手动 usbdisplay_send 输出将不再具备权限
不会改动音频配置。

EOF
  local answer
  read -r -p "继续卸载？[y/N] " answer
  if [[ ! "$answer" =~ ^[Yy]$ ]]; then
    log "已取消"
    return 0
  fi
  uninstall_user_service "$user" "$home"
  stop_evdi_unit_or_die
  rm -f "$DAEMON_BIN" "$GRABBER_BIN" "$CTL_BIN"
  "$ROOT/tools/install-evdi-support.sh" --uninstall
  "$ROOT/tools/install-video-rules.sh" --uninstall
  log "卸载完成；evdi 设备保留到重启或模块重新加载"
}

main() {
  local uninstall_mode=0
  local skip_confirm=0
  while [[ $# -gt 0 ]]; do
    case "$1" in
      --uninstall) uninstall_mode=1 ;;
      --yes|-y) skip_confirm=1 ;;
      --help|-h) usage; exit 0 ;;
      *) printf '未知参数：%s\n' "$1" >&2; usage >&2; exit 2 ;;
    esac
    shift
  done

  require_root
  check_systemd

  local user home
  user="$(user_name)"
  home="$(user_home "$user")"
  if [[ "$uninstall_mode" == "1" ]]; then
    if [[ "$skip_confirm" == "1" ]]; then
      # 使用 yes 时跳过 read，但保留同样的提示信息。
      uninstall_all_yes "$user" "$home"
    else
      uninstall_all "$user" "$home"
    fi
  else
    install_all "$user" "$home"
  fi
}

# 与 uninstall_all 相同，只是跳过交互确认。
uninstall_all_yes() {
  local user="$1"
  local home="$2"
  warn "卸载会移除 USB/HID 视频权限规则，手动 usbdisplay_send 输出将不再具备权限；不会改动音频配置"
  log "卸载 user 服务、daemon、evdi 支持与视频权限规则（已指定 --yes）"
  uninstall_user_service "$user" "$home"
  stop_evdi_unit_or_die
  rm -f "$DAEMON_BIN" "$GRABBER_BIN" "$CTL_BIN"
  "$ROOT/tools/install-evdi-support.sh" --uninstall
  "$ROOT/tools/install-video-rules.sh" --uninstall
  log "卸载完成；evdi 设备保留到重启或模块重新加载"
}

main "$@"
