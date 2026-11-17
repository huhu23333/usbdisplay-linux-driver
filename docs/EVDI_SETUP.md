# evdi 虚拟显示支持

本文档说明如何为 USB Display 准备一个专用的 evdi 虚拟显示设备。该设备固定为
1920x1080，仅用于视频输出，evdi 路径不使用音频。安装阶段需要 sudo；grabber 作为
systemd 服务运行，消费 daemon 使用普通用户权限。

本项目的脚本只创建并标记自己的 evdi 设备，不枚举、不打开、不修改、不删除
机器上已有的 evdi 设备，也不会调用 `/sys/devices/evdi/remove_all`。

## 组成

- `tools/evdi-precreate.sh`：root 辅助脚本，创建或复用专用设备，并写入 marker。
- `tools/usbdisplay-evdi.service`：systemd 常驻服务，`ExecStartPre` 预创建/复用 card，
  `ExecStart` 启动 `usbdisplay_evdi_grabber`；两者同生命周期，grabber 不能单独启动。
- `tools/install-evdi-support.sh`：安装/卸载 unit、预创建脚本、Xorg 配置，检查 grabber，
  创建 `usbdisplay` 组并授权共享内存。
- `tools/usbdisplay-daemon.service`：消费 daemon 的 user unit，登录后读取共享帧并编码发送。
- `impl/tools/usbdisplay_evdi_grabber.cpp`：只抓帧写 `/dev/shm/usbdisplay-frames`。
- `impl/tools/usbdisplay_daemon.cpp`：只读共享帧，不直接访问 evdi。
- `install.sh`：一键安装视频规则、evdi 支持、grabber、daemon 与用户服务。
- `/run/usbdisplay/evdi-card`：本服务创建的 marker，权限 0644，grabber 只读。

marker 格式为 key=value：

```
card=/dev/dri/card5
card_name=card5
connector=DVI-I-5
width=1920
height=1080
owner=usbdisplay-linux-driver
platform=/sys/devices/platform/evdi.1
created=2025-01-01T00:00:00+00:00
```

grabber 只读取该文件，并校验 `owner=usbdisplay-linux-driver`、platform、driver 与
connector，不枚举 `/sys/class/drm`，也不抢占其他 evdi 节点。消费 daemon 不读取
marker，只读共享内存。

## 安装

```bash
sudo tools/install-evdi-support.sh
```

安装脚本会（需要 grabber 已经由 `install.sh` 或 cmake 构建安装）：

1. 检测 `modinfo evdi`。缺失时按发行版尝试安装 `evdi-dkms`；失败时打印手动安装指引并停止。
2. 把预创建脚本和 unit 安装到 `/usr/local/libexec/usbdisplay/`，并在
   `/etc/systemd/system/usbdisplay-evdi.service` 建立指向 unit 的符号链接。
3. 创建 `usbdisplay` 组并把 sudo 用户加入，安装 Xorg 配置与 grabber 引用。
4. 执行 `systemctl daemon-reload`；检测到图形会话时只 `enable`，不立即启动
   （重启后由 ExecStartPre 预创建并拉起 grabber）；无图形会话时 `enable --now`。
5. 读取 marker/共享帧路径，打印 card、连接器与权限提示。

选项：

```bash
sudo tools/install-evdi-support.sh --no-module-install   # 不自动安装 evdi 模块
sudo tools/install-evdi-support.sh --uninstall           # 卸载本项目的 unit、脚本与 marker
```

安装脚本不修改 `/etc/modprobe.d`，不加载/卸载已有模块配置，也不会删除已有设备。

### 模块安装

安装脚本会优先尝试 `evdi-dkms`。不同发行版的包名和前提条件不同，失败时按下面
方式手动处理。

Ubuntu/Debian：

```bash
sudo apt install dkms linux-headers-$(uname -r) evdi-dkms
```

如果仓库没有 `evdi-dkms`，需要先添加提供该包的第三方仓库（例如 DisplayLink
相关仓库），再重试。Secure Boot 开启时 DKMS 需要注册 MOK：

```bash
mokutil --sb-state
sudo mokutil --import /var/lib/shim-signed/mok/MOK.der   # 路径以实际 DKMS 密钥为准
# 重启，在 MOK 界面完成注册后再检查
```

Fedora（通常来自 RPMFusion）：

```bash
sudo dnf install akmod-evdi evdi
```

Arch：

```bash
sudo pacman -S dkms linux-headers
# evdi-dkms 通常来自 AUR，例如：
yay -S evdi-dkms
```

通用源码构建：

```bash
git clone https://github.com/DisplayLink/evdi.git
cd evdi
make
sudo make install
sudo depmod -a
```

加载与检查：

```bash
sudo modprobe evdi
lsmod | grep evdi
modinfo evdi
ls -l /sys/devices/evdi/add
```

加载后应出现 `/sys/devices/evdi/add`。没有该文件通常说明模块未正确安装、DKMS
没有为当前内核构建，或 Secure Boot 拒绝了未签名模块。

## 专用设备所有权模型

本服务启动时：

1. 若 `/run/usbdisplay/evdi-card` 存在且 card、driver、connector 均有效，直接复用。
2. 否则记录创建前的 `/sys/class/drm/card*` 集合。
3. 向 `/sys/devices/evdi/add` 写入 1，最多等待 5 秒出现新的 card。
4. 只检查新增的 card，确认 driver 为 evdi，找到其连接器。
5. 把 card 节点与连接器写入 marker（0644），grabber 后续只读 marker。

因此，本机已有的 evdi 设备（无论由 DisplayLinkManager、其他程序还是手工创建）
不会被本服务使用或修改。grabber 不得遍历其他 evdi 设备，也不得调用
`remove_all` 或删除设备节点。卸载本项目移除 unit、脚本、grabber、共享帧与自有
marker/Xorg 配置，设备本身保留到重启或模块重新加载。

## 显示出现与消失（方案一）

本方案不按 USB 插拔创建或销毁 evdi 设备：

- 服务在开机时创建专用虚拟显示设备并保持常驻，初始状态为 disconnected。
- grabber 读取 marker 与稳定链接，检测 USB 显示设备（1de1:f201）是否存在：
  - 插入时调用 `evdi_connect2` 提交 1920x1080 的 EDID，内核发出热插拔事件，合成器把该显示器加入桌面，显示设置中出现；
  - 拔出时调用 `evdi_disconnect` 或关闭句柄，内核发出拔出事件，显示器从桌面消失。
- grabber 由 `usbdisplay-evdi.service` 常驻运行并维持连接；grabber 退出或异常结束会断开，显示器消失；systemd 重启后重新连接。
- 未插入时 evdi card 与 disconnected 连接器仍然存在，`xrandr --query` 会列出 `DVI-I-N disconnected`；KDE/GNOME 的显示设置通常只列出已连接显示器，因此不会出现该设备。这是方案一的预期取舍，用于避免按插拔创建或销毁设备、触碰他人设备池。
- 显示固定 1920x1080；evdi 路径不使用音频。
- 可用 `sudo tools/evdi-precreate.sh --status` 查看 USB 是否插入与连接器当前状态。

## grabber、共享帧与消费 daemon

手动视频输出与扩展显示屏是两层独立功能：

- 手动路径：安装 USB/HID 视频权限规则后，普通用户使用 `usbdisplay_send` 或 C++
  Controller 直接发送 H.264，不依赖 evdi。
- 扩展显示屏：单个 `usbdisplay-evdi.service` 先用 `ExecStartPre` 预创建/复用专用
  evdi card，再常驻启动 `usbdisplay_evdi_grabber`。grabber 只抓帧写共享内存；
  消费 daemon 只读共享内存，编码后经 Controller 发送。消费 daemon 不直接访问 evdi。

### 统一生命周期

```
usbdisplay-evdi.service
  ExecStartPre = evdi-precreate.sh      创建/复用 card、marker
  ExecStart    = usbdisplay_evdi_grabber 常驻抓帧
```

grabber 与 card 同生命周期，不能单独启动；单独运行 grabber 会因缺少 systemd
`INVOCATION_ID` 拒绝启动。正常停止时 grabber 先 `evdi_disconnect`，再关闭 card
与共享内存后退出；systemd `Restart=always`、`RestartSec=2`，`TimeoutStopSec=10`。
grabber 连接后 3 秒内没有成功 grab、事件循环异常或共享帧发布持续失败时主动断开
并退出，让 systemd 重建。

### 控制 socket

- `/run/usbdisplay/control.sock`（0660、组 `usbdisplay`）由 grabber 创建；服务默认
  disconnected，不自动连接 EDID。任何程序可发送文本行 `CONNECT`/`DISCONNECT`/
  `STATUS`，回复 `OK connected=...` 或 `ERR <reason>`。
- CONNECT 一律提交同时含 1080p60（148.5MHz）与 1080p120（297MHz、2200x1125）
  两个 DTD 的 EDID，pixel_per_second 按 120；可选 `CONNECT 60`/`CONNECT 120`
  只做参数校验，不再改变 EDID，切换刷新率由桌面直接选择模式完成。
- STATUS 返回 `OK connected=0|1 width=1920 height=1080 modes=60,120`；
  非法参数返回 `ERR unsupported-fps`，CONNECT 幂等。
- `usbdisplay_evdi_ctl connect [60|120]|disconnect|status [--socket PATH]` 为 CLI，
  退出码 0 成功、1 错误、2 用法。CONNECT 失败状态保持 disconnected；重复调用幂等。
- USB 消费 daemon 自启动，只在 USB 在线时调用 CONNECT、离线时调用 DISCONNECT；
  控制接口不可用时重试，不影响 grabber 抓帧。服务重启后状态不持久化。

### 共享帧

- 共享帧路径 `/dev/shm/usbdisplay-frames`，0660、组 `usbdisplay`；grabber 以临时名
  初始化并原子 rename 发布，reader 以 `O_RDONLY` 打开并 `PROT_READ` 映射，绝不写映射。
- 租约 socket `/run/usbdisplay/frames.sock`，0660；systemd unit 设置
  `RuntimeDirectory=usbdisplay`。协议为固定小端二进制 ACQUIRE/RELEASE，
  server 在 grabber 主循环中非阻塞服务，单轮处理有限消息。
- 固定 4096 字节头 + 3 个完整 XRGB8888 槽，1920x1080、stride 7680。header 含
  magic/version/header_size/width/height/stride/format/slot_count/slot_stride/
  generation/heartbeat/writer_pid/latest_token 与槽状态。
- writer 只发布 FREE/READY 且未租用的槽；server 单方管理 READY→READING→FREE。
  ACQUIRE 返回 token 和布局/offset；RELEASE 校验 token，过期/双 release 被忽略；
  连接断开立即释放该连接租约，consumer 崩溃不会永久占槽。
- reader 复制到本地缓冲后再编码；socket 断开、共享 inode 变化、generation/尺寸变化
  时重开映射与 socket。没有可用槽返回 EMPTY，慢 consumer 不阻塞 grabber。

### 编码与发送

消费 daemon 在 USB 在线时启动 Controller（video only，`enable_audio=false`），
USB 发送固定 60fps：每 1/60 秒最多取一次最新源帧编码发送，源 60/120fps 的中间帧丢弃，
无新帧不重复发送；PTS 用发送序号 `sent_frames*1e6/60`。
从共享帧取最新完整帧，经 libswscale 转 I420，libx264 使用 superfast、zerolatency、
main profile、level 4.2、bframes=0、ref=1、keyint=120、scenecut=0、aud=1、
repeat-headers=1、annexb=1、4 Mbps（maxrate 4.2 Mbps、bufsize 4 Mbps）编码；
PTS 按发送序号 × 1/60 秒，关键帧来自 x264 `b_keyframe`。无新帧不重复发送。
USB 离线时停止 Controller；generation 变化时重置编码器；发送失败时先关闭共享
读取端，再停止 Controller，避免解引用已销毁对象。

### 安装与卸载

```bash
sudo ./install.sh
sudo ./install.sh --uninstall
```

一键脚本安装 USB/HID 视频权限规则、构建并安装 grabber 与 daemon、调用
`tools/install-evdi-support.sh` 安装 unit/预创建脚本/Xorg 配置，并安装消费 daemon
user unit。安装检测到图形会话时只 `enable` evdi 服务，不立即启动（需重启生效）；
安装不会提前启动用户 daemon，登录桌面后执行
`systemctl --user start usbdisplay-daemon.service`。它不安装音频隔离规则，不修改
音频配置，evdi 路径不使用音频。

卸载会停止 grabber 与 user daemon，移除 unit、脚本、Xorg 配置、grabber、daemon、
共享帧文件与自建 `usbdisplay` 组（仅当组为空）；不删除 evdi 设备、不卸载模块、
不触碰其他 evdi 设备。

用户服务操作：

```bash
systemctl --user status usbdisplay-daemon.service
systemctl --user restart usbdisplay-daemon.service
journalctl --user -u usbdisplay-daemon.service -b
journalctl -u usbdisplay-evdi.service -b
```

Wayland/KWin 多 GPU 限制见下文。

## 显示集成

虚拟显示固定为 1920x1080。grabber 按这个尺寸抓帧并写入共享帧；X11/Wayland 侧
还需要把该 card 的输出启用并设置模式。

### X11

先确认 provider 与输出名称：

```bash
xrandr --listproviders
xrandr --query
```

evdi 输出通常由 modesetting 驱动承载。可按实际 provider 名称把 evdi provider
连接到主 GPU：

```bash
xrandr --setprovideroutputsource <evdi-provider> <primary-provider>
```

如果输出没有自动被识别，可添加仅针对该驱动的 OutputClass 片段，例如
`/etc/X11/xorg.conf.d/20-usbdisplay-evdi.conf`：

```ini
Section "OutputClass"
    Identifier "usbdisplay-evdi"
    MatchDriver "evdi"
    Driver "modesetting"
EndSection
```

然后用 xrandr 把输出设为固定模式。1920x1080@60 的常用 Modeline：

```bash
xrandr --newmode "1920x1080_60" 148.50 1920 2008 2052 2200 1080 1084 1089 1125 +hsync +vsync
xrandr --addmode <connector> "1920x1080_60"
xrandr --output <connector> --mode "1920x1080_60" --right-of <primary-output>
```

实际 provider、connector 名称以本机 `xrandr` 与 marker 为准。

### Wayland

KWin（KDE Plasma）默认可能只使用主 GPU。需要把 evdi card 与主 GPU 一起列入
`KWIN_DRM_DEVICES`，例如：

```bash
KWIN_DRM_DEVICES=/dev/dri/card1:/dev/dri/card5 kwin_wayland
```

具体 card 编号使用 marker 中的值，主 GPU 使用 `/sys/class/drm` 中对应显示输出
的 card。KWin 的多 GPU 输出启用方式随版本变化，必要时在 KDE 显示设置中启用
evdi 输出并设置为 1920x1080。

Mutter/GNOME 对 evdi 的支持有限，可能出现输出不出现、模式列表为空或格式协商
失败。遇到问题时优先使用 X11 会话或 KWin，并确认 evdi 输出没有被 Mutter 当作
未知 GPU 忽略。

## 排障

只读检查：

```bash
sudo tools/evdi-precreate.sh --status
systemctl status usbdisplay-evdi.service
journalctl -u usbdisplay-evdi.service -b
```

常见问题：

- `modprobe evdi` 失败：检查 DKMS 是否为当前内核构建、内核头文件是否匹配、
  Secure Boot 的 MOK 是否已注册。见上文模块安装。
- 服务成功但 marker 不存在：查看 journal；确认 `/sys/devices/evdi/add` 可写。
- 没有新 card：evdi 模块可能未正确加载，或并发创建失败；已有设备不会被修改。
- marker 无效：可能 card 已随重启消失；重新执行预创建会分配新设备，旧设备不删除。
- 普通用户打不开 `/dev/dri/cardN`：把用户加入 video 组并重新登录，或使用 ACL：

  ```bash
  sudo usermod -aG video <user>
  # 或
  sudo setfacl -m u:<user>:rw /dev/dri/cardN
  ```

- KDE 设置中看不到输出：检查 `KWIN_DRM_DEVICES` 是否包含 evdi card，并确认
  显示管理器在服务之后启动。
- 输出模式无法设置：确认使用 1920x1080 Modeline，且 connector 名称来自 marker。

## 持有者描述与清理

evdi 的 `/sys/devices/evdi/add` 只接受设备数量或 `usb:` 路径，没有 owner 或 name 参数；内核也不提供自定义 card 名称或单设备删除接口，只有 `remove_all`。因此无法在创建请求本身附加持有者描述。若写入 `usb:总线-端口` 形式，evdi 会把虚拟设备挂到指定 USB 设备下，形成 sysfs 父子关系并在该 USB 设备移除时自动清理；本项目当前未采用这种挂接方式，需要实机验证设备行为。

本项目采用用户态登记补足：

- marker 中写入 `owner=usbdisplay-linux-driver`、`platform=/sys/devices/platform/evdi.N` 与 `created` 时间戳；
- 创建稳定链接 `/dev/dri/usbdisplay-evdi` 指向本项目 card；
- `--status` 把本项目设备标为“本项目专用”，其他 evdi 设备标为“未登记，可能属于其他程序”。

grabber 只读 marker 与稳定链接，不枚举其他 evdi 节点。人工清理前执行 `sudo tools/evdi-precreate.sh --status` 可以看到归属；卸载本项目只删除自己的 marker 与稳定链接，不删除设备。evdi 没有单设备删除接口，`remove_all` 会删除机器上所有 evdi 设备（包括其他程序的），不要使用；需要立即清除时只能重启或重载 evdi 模块，这会影响所有使用者。

grabber 连接时在 EDID 中写入监视器名称 `USBDisplay`，KDE/GNOME 显示设置里会显示该名称；它属于显示设备信息，不是内核持有者字段。

## 卸载限制

```bash
sudo tools/install-evdi-support.sh --uninstall
```

卸载会停止并移除本项目的 unit、预创建脚本、grabber、共享帧文件、自有 marker、
Xorg 配置与自建 `usbdisplay` 组（仅当组为空），但：

- 不卸载 evdi 模块；
- 不删除任何 `/dev/dri/cardN` 设备节点；
- 不调用 `remove_all`；
- 不触碰其他程序创建或当前机器已有的 evdi 设备。

已创建的专用设备会保留到重启或模块重新加载。若需要清除该设备，只能在确认不会
影响其他 evdi 使用者后，由管理员自行重启或重新加载模块。

## 与 DisplayLinkManager 共存

DisplayLinkManager 等程序可能也使用 evdi 并创建多个虚拟设备。本服务不假设 evdi
设备数量，也不使用 `remove_all`，只根据自己 marker 中的 card 与 connector 工作。
两者可以共存：其他程序新增或删除设备不会改变本项目的 marker（除非 marker 指向
的设备本身消失）。安装本服务不会修改 evdi 的 modprobe 参数或其他程序的 unit。
