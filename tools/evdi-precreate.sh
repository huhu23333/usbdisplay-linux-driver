#!/usr/bin/env bash
# evdi 专用虚拟显示设备预创建辅助脚本（需要 root 创建设备）。
#
# 用法：
#   sudo tools/evdi-precreate.sh             创建或复用本项目的 evdi 设备
#   sudo tools/evdi-precreate.sh --status    只读显示模块、marker 与现有 evdi 设备
#   tools/evdi-precreate.sh --status         普通用户可执行只读查询
#
# 本脚本只向 /sys/devices/evdi/add 写入一次，绝不调用 remove_all，
# 不打开、修改或删除机器上已有的 evdi 设备，也不修改 /etc/modprobe.d。
set -euo pipefail

MARKER_DIR="/run/usbdisplay"
MARKER="$MARKER_DIR/evdi-card"
ADD_PATH="/sys/devices/evdi/add"
DRM_ROOT="/sys/class/drm"
WIDTH=1920
HEIGHT=1080
OWNER="usbdisplay-linux-driver"
LINK="/dev/dri/usbdisplay-evdi"

log() { printf '[usbdisplay-evdi] %s\n' "$*"; }
warn() { printf '[usbdisplay-evdi] 警告：%s\n' "$*" >&2; }
die() { printf '[usbdisplay-evdi] 错误：%s\n' "$*" >&2; exit 1; }

print_usage() {
  cat <<'EOF'
用法：
  sudo tools/evdi-precreate.sh             创建或复用一个专用 evdi 虚拟显示设备
  sudo tools/evdi-precreate.sh --status    只读查看模块、marker 与现有 evdi 设备
  tools/evdi-precreate.sh --status         普通用户可执行只读查询

成功创建后写入 /run/usbdisplay/evdi-card（0644），内容为 key=value：
  card=/dev/dri/cardN
  card_name=cardN
  connector=DVI-I-N
  width=1920
  height=1080
  owner=usbdisplay-linux-driver
  platform=/sys/devices/platform/evdi.N
  created=时间戳
同时创建稳定链接 /dev/dri/usbdisplay-evdi 指向该 card。
已有 evdi 设备不会被枚举、打开、修改或删除。
EOF
}

existing_cards() {
  local path name
  for path in "$DRM_ROOT"/card[0-9]*; do
    [[ -e "$path" ]] || continue
    name="${path##*/}"
    [[ "$name" =~ ^card[0-9]+$ ]] || continue
    printf '%s\n' "$name"
  done | sort
}

card_driver() {
  local card="$1"
  local link
  link="$(readlink -f "$DRM_ROOT/$card/device/driver" 2>/dev/null || true)"
  [[ -n "$link" ]] || return 0
  printf '%s\n' "${link##*/}"
}

card_connectors() {
  local card="$1"
  local path name
  for path in "$DRM_ROOT/$card"-*; do
    [[ -e "$path" ]] || continue
    name="${path##*/}"
    [[ "$name" == "$card-"* ]] || continue
    printf '%s\n' "${name#"$card-"}"
  done | sort
}

usb_devices() {
  local path
  for path in /sys/bus/usb/devices/*; do
    [[ -f "$path/idVendor" && -f "$path/idProduct" ]] || continue
    if [[ "$(cat "$path/idVendor" 2>/dev/null)" == "1de1" && \
          "$(cat "$path/idProduct" 2>/dev/null)" == "f201" ]]; then
      printf '%s\n' "$path"
    fi
  done
}

marker_get() {
  local key="$1"
  [[ -f "$MARKER" ]] || return 0
  awk -F= -v key="$key" '$1 == key { sub(/^[^=]*=/, ""); print; exit }' "$MARKER"
}

marker_valid() {
  [[ -f "$MARKER" ]] || return 1
  local card connector name driver
  card="$(marker_get card)"
  connector="$(marker_get connector)"
  [[ -n "$card" && -n "$connector" ]] || return 1
  [[ "$card" == /dev/dri/card* ]] || return 1
  name="${card##*/}"
  [[ -e "$DRM_ROOT/$name" ]] || return 1
  driver="$(card_driver "$name")"
  [[ "$driver" == "evdi" ]] || return 1
  [[ -e "$DRM_ROOT/$name-$connector" ]] || return 1
  local owner platform actual_platform
  owner="$(marker_get owner)"
  [[ "$owner" == "$OWNER" ]] || return 1
  platform="$(marker_get platform)"
  [[ -n "$platform" ]] || return 1
  actual_platform="$(readlink -f "$DRM_ROOT/$name/device" 2>/dev/null || true)"
  [[ "$actual_platform" == "$platform" ]] || return 1
  return 0
}

print_status() {
  local loaded_version count
  printf '[usbdisplay-evdi] 状态（只读）\n'
  if command -v modinfo >/dev/null 2>&1 && modinfo evdi >/dev/null 2>&1; then
    loaded_version="$(modinfo -F version evdi 2>/dev/null || true)"
    printf '  模块：已安装'
    [[ -n "$loaded_version" ]] && printf '（版本 %s）' "$loaded_version"
    printf '\n'
  else
    printf '  模块：未安装或 modinfo 不可用\n'
  fi
  if [[ -e /sys/module/evdi ]]; then
    printf '  /sys/module/evdi：存在\n'
  else
    printf '  /sys/module/evdi：不存在（模块未加载）\n'
  fi
  if [[ -e "$ADD_PATH" ]]; then
    printf '  %s：存在\n' "$ADD_PATH"
  else
    printf '  %s：不存在\n' "$ADD_PATH"
  fi
  if [[ -r /sys/devices/evdi/count ]]; then
    count="$(cat /sys/devices/evdi/count 2>/dev/null || true)"
    printf '  evdi 设备计数：%s\n' "${count:-未知}"
  fi
  printf '  DRM 设备：\n'
  local card driver connectors connector managed_card managed_name managed_owner
  managed_card="$(marker_get card)"
  managed_name="${managed_card##*/}"
  managed_owner="$(marker_get owner)"
  for card in $(existing_cards); do
    driver="$(card_driver "$card")"
    connectors="$(card_connectors "$card" | paste -sd, -)"
    if [[ "$driver" == "evdi" ]]; then
      if [[ -n "$managed_name" && "$card" == "$managed_name" ]]; then
        printf '    %s driver=%s connectors=%s（本项目专用，owner=%s）\n' \
          "$card" "${driver:-未知}" "${connectors:-无}" "${managed_owner:-未知}"
      else
        printf '    %s driver=%s connectors=%s（未登记，可能属于其他程序，不会被修改）\n' \
          "$card" "${driver:-未知}" "${connectors:-无}"
      fi
    else
      printf '    %s driver=%s connectors=%s\n' "$card" "${driver:-未知}" "${connectors:-无}"
    fi
  done
  local usb_count
  usb_count="$(usb_devices | wc -l)"
  if [[ "$usb_count" -gt 0 ]]; then
    printf '  USB 显示设备：已插入（%s）\n' "$(usb_devices | paste -sd, -)"
  else
    printf '  USB 显示设备：未插入；虚拟显示保持断开，桌面显示设置中不会出现\n'
  fi
  if [[ -f "$MARKER" ]]; then
    printf '  marker：%s\n' "$MARKER"
    sed 's/^/    /' "$MARKER"
    if marker_valid; then
      printf '  marker 状态：有效，可复用\n'
    else
      printf '  marker 状态：无效，创建时会重新分配\n'
    fi
    if [[ -L "$LINK" ]]; then
      printf '  稳定链接：%s -> %s\n' "$LINK" "$(readlink "$LINK" 2>/dev/null || true)"
    else
      printf '  稳定链接：%s 不存在\n' "$LINK"
    fi
    local marker_card marker_connector connector_state
    marker_card="$(marker_get card)"
    marker_connector="$(marker_get connector)"
    if [[ -n "$marker_card" && -n "$marker_connector" ]]; then
      connector_state="$(cat "$DRM_ROOT/${marker_card##*/}-$marker_connector/status" 2>/dev/null || true)"
      printf '  虚拟显示连接状态：%s\n' "${connector_state:-未知}"
    fi
  else
    printf '  marker：不存在（尚未创建专用设备）\n'
  fi
}

graphical_session_active() {
  if command -v loginctl >/dev/null 2>&1; then
    local id type state sessions
    if ! sessions="$(loginctl list-sessions --no-legend 2>/dev/null)"; then
      return 0  # 查询失败时保守视为活动会话
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
  # 无法查询 logind 时保守视为有图形会话，避免热插拔。
  if pgrep -x Xorg >/dev/null 2>&1 || pgrep -x kwin_wayland >/dev/null 2>&1 || \
     pgrep -x gnome-shell >/dev/null 2>&1 || pgrep -x weston >/dev/null 2>&1 || \
     pgrep -x sway >/dev/null 2>&1 || [[ -e /tmp/.X11-unix/X0 ]]; then
    return 0
  fi
  return 0
}

write_marker() {
  local card_name="$1" connector="$2" platform="$3" created="$4"
  install -d -m 0755 "$MARKER_DIR"
  local tmp
  tmp="$(mktemp "$MARKER_DIR/.evdi-card.XXXXXX")"
  trap 'rm -f "${tmp:-}"' EXIT
  {
    printf 'card=/dev/dri/%s\n' "$card_name"
    printf 'card_name=%s\n' "$card_name"
    printf 'connector=%s\n' "$connector"
    printf 'width=%s\n' "$WIDTH"
    printf 'height=%s\n' "$HEIGHT"
    printf 'owner=%s\n' "$OWNER"
    printf 'platform=%s\n' "$platform"
    printf 'created=%s\n' "$created"
  } > "$tmp"
  chmod 0644 "$tmp"
  mv -f "$tmp" "$MARKER"
  trap - EXIT
}

create_device() {
  if [[ "${EUID:-$(id -u)}" -ne 0 ]]; then
    die "创建 evdi 设备需要 root，请使用：sudo $0"
  fi

  if marker_valid; then
    local existing_card existing_name
    existing_card="$(marker_get card)"
    existing_name="${existing_card##*/}"
    for ((i = 0; i < 50; ++i)); do
      [[ -e "$existing_card" ]] && break
      sleep 0.1
    done
    if [[ ! -e "$existing_card" ]]; then
      warn "marker 中的 $existing_card 暂未出现，仍按 sysfs 复用该设备"
    fi
    if ln -sfn "$existing_name" "$LINK" 2>/dev/null; then
      log "稳定链接：$LINK -> $existing_name"
    else
      warn "无法创建稳定链接 $LINK"
    fi
    log "复用已有专用 evdi 设备"
    print_status
    return 0
  fi

  # 服务模式由 systemd 在显示管理器之前调用，只检查 DM 是否已经启动；
  # 手动模式检查活动图形会话。这样 boot 早期不依赖 logind 会话状态。
  if [[ "${SERVICE_MODE:-0}" == "1" ]]; then
    if systemctl is-active --quiet display-manager.service 2>/dev/null; then
      die "显示管理器已启动，拒绝新增 evdi GPU；请重启系统"
    fi
  elif graphical_session_active; then
    die "图形会话正在运行，拒绝新增 evdi GPU；请重启系统，由 usbdisplay-evdi.service 在显示管理器之前创建"
  fi

  if [[ -f "$MARKER" ]]; then
    # 不根据不完整 marker 认领已有 evdi card，避免把其他程序的设备当成
    # 本项目设备。marker 无效时 fail closed，重启后由 /run 清空重建。
    die "已有 marker 无效，拒绝自动认领已有 evdi 设备；请重启系统或手动移除 $MARKER 后重试"
  fi

  if [[ ! -e "$ADD_PATH" ]]; then
    command -v modprobe >/dev/null 2>&1 || die "找不到 modprobe；请先安装 evdi 模块"
    log "加载 evdi 模块"
    if ! modprobe evdi 2>/dev/null; then
      die "modprobe evdi 失败。请先安装 evdi-dkms 并确认 Secure Boot MOK（见 docs/EVDI_SETUP.md），然后执行 sudo modprobe evdi"
    fi
  fi
  [[ -e "$ADD_PATH" ]] || die "evdi 已加载但缺少 $ADD_PATH；请确认 evdi 版本"

  local before after new_card card_node connector
  before="$(existing_cards)"
  log "向 $ADD_PATH 写入 1，创建专用 evdi 设备"
  if ! printf '1\n' > "$ADD_PATH" 2>/dev/null; then
    die "写入 $ADD_PATH 失败；未修改任何已有设备"
  fi

  for ((i = 0; i < 50; ++i)); do
    after="$(existing_cards)"
    new_card=""
    for card_node in $after; do
      if ! printf '%s\n' "$before" | grep -Fxq "$card_node"; then
        if [[ "$(card_driver "$card_node")" == "evdi" ]]; then
          new_card="$card_node"
          break
        fi
      fi
    done
    [[ -n "$new_card" ]] && break
    sleep 0.1
  done
  [[ -n "${new_card:-}" ]] || die "创建请求已提交，但 5 秒内未发现新的 evdi card；已有设备保持不变"

  card_node="/dev/dri/$new_card"
  for ((i = 0; i < 50; ++i)); do
    [[ -e "$card_node" ]] && break
    sleep 0.1
  done
  [[ -e "$card_node" ]] || die "新 card $new_card 已出现，但 $card_node 未就绪；未修改已有设备"

  connector=""
  for ((i = 0; i < 50; ++i)); do
    connector="$(card_connectors "$new_card" | head -n1)"
    [[ -n "$connector" ]] && break
    sleep 0.1
  done
  [[ -n "$connector" ]] || die "新 evdi card $new_card 未出现连接器；未写入 marker"

  local platform created card_device
  platform="$(readlink -f "$DRM_ROOT/$new_card/device" 2>/dev/null || true)"
  card_device="$(readlink -f "$DRM_ROOT/$new_card/device" 2>/dev/null || true)"
  [[ -n "$platform" && -e "$platform" && "$platform" == "$card_device" ]] ||
    die "card $new_card 的 platform/device realpath 校验失败；未写入 marker"
  created="$(date -Is 2>/dev/null || date)"

  write_marker "$new_card" "$connector" "$platform" "$created"

  if ln -sfn "$new_card" "$LINK" 2>/dev/null; then
    log "稳定链接：$LINK -> $new_card"
  else
    warn "无法创建稳定链接 $LINK"
  fi

  log "已创建专用 evdi 设备（owner=$OWNER）"
  sed 's/^/  /' "$MARKER"
  log "后续使用 marker：daemon 只读 $MARKER，不枚举其他 evdi 设备"
}

SERVICE_MODE=0
case "${1:-}" in
  --service)
    SERVICE_MODE=1
    create_device
    ;;
  --status)
    print_status
    ;;
  "")
    create_device
    ;;
  --help|-h)
    print_usage
    ;;
  *)
    printf '未知参数：%s\n' "$1" >&2
    print_usage >&2
    exit 2
    ;;
esac
