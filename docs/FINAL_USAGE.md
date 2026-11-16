# USB_DISPLAY Linux 用户态投屏 —— 最终使用说明

已验证环境：Ubuntu 22.04（内核 6.8），设备 USB_DISPLAY 1de1:f201。
无需修改内核/系统文件；编码器可用任意机器上的 x264 软件编码。

## 1. 已验证的“设备兼容 H.264 编码格式”（逆向结论）

| 项 | 要求 |
|---|---|
| 封装 | Annex-B 裸流；每个 AU 以 AUD（00 00 00 01 09 …）开头，保留起始码 |
| Profile / Level | Main (77) / <=4.2 |
| 熵编码 | CABAC |
| B 帧 | 禁止（bframes=0 -> pic_order_cnt_type=2） |
| 参考帧 | ref=1，max_num_reorder_frames=0，max_dec_frame_buffering=1 |
| 分辨率 | 1920x1080（设备 EDID 输出） |
| 帧率 | 建议 60fps（不要用低帧率大帧） |
| 码率 | 3.5–4.5 Mbps，帧大小尽量均匀 |
| HRD/SEI | 非必需（x264 不带也流畅；Windows QSV 带 HRD/pic_timing） |

不满足时：起始码缺失 = 黑屏；带 B 帧 = 能显示但周期性抽动。

## 2. 一次性安装（之后无需 sudo / chmod / 拔插）

    cd /media/huhu233/Data2/projects2/DSH/reverse
    sudo tools/install.sh          # 写入 udev 规则并 reload/trigger

安装后：
- 普通用户可直接访问设备（无需 sudo/chmod）；
- usbdisplay_send 默认启用 --auto-reset：会话开始或握手失败时自动
  libusb_reset_device -> 等待重枚举 -> 重试完整握手，等效于拔插。

真机验收（已通过）：连续两次运行
./build/usbdisplay_send --live --h264 stream.h264，中间不拔插、不用 sudo，
两次均 isAllowed 成功、各 301 帧 0 错误。

卸载：sudo tools/install.sh --uninstall

## 3. 编码（任意机器）

默认用 x264（可移植，任何 Linux/Windows 机器都能跑）：

    ffmpeg -y -i input.mp4 -c:v libx264 -profile:v main -level:v 4.2 \
      -pix_fmt yuv420p -b:v 4M -maxrate 4200k -bufsize 4000k \
      -x264-params "aud=1:repeat-headers=1:bframes=0:ref=1:keyint=120:scenecut=0" \
      -f h264 stream.h264

或用封装脚本（默认 x264，可加 --vaapi 用 Intel 核显）：

    tools/usbdisplay_encode.sh input.mp4 stream.h264 [fps=60] [bitrate=4M] [--vaapi]

> 本机若用 Intel 核显硬编（可选）：驱动已编译在 work/va2/，
> export LD_LIBRARY_PATH=$PWD/work/va2/lib LIBVA_DRIVERS_PATH=$PWD/work/va2/lib/dri LIBVA_DRIVER_NAME=iHD

## 4. 投屏

    ./build/usbdisplay_send --live --h264 stream.h264
    # 或使用封装脚本：
    tools/usbdisplay_run.sh stream.h264
    # 带音频（WAV，经设备 USB Audio 播放）：
    ./build/usbdisplay_send --live --h264 stream.h264 --wav audio.wav

- 帧率自动从 SPS VUI 读取，单次顺序播放；
- 默认完整握手并等待 isAllowed，失败自动复位重试（--no-auto-reset 可关闭）；
- --stats 实时统计，--trace info 日志。

## 5. 已知限制

- 视频/音频走不同 USB 通道，音画同步为各自按时间播放，无设备级 A/V 同步协议。
- 设备只接受新枚举后的会话；程序已用 --auto-reset 自动处理，无需人工拔插。
- 旧 CLI 参数（--frame-mode、--a3/--a4、inner-header 等）已废弃。
- 协议细节与全部实测证据见 USB_PROTOCOL_VERIFIED.md。
