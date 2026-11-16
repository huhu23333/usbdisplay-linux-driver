# USB_DISPLAY (1de1:f201) 协议实测结论（Linux 用户态）

> 本文所有结论均来自 **1.pcapng（Windows 正常投屏全过程 USB 抓包）** 与真机实测，
> 取代此前基于反编译的推测版本。

## 1. 设备与拓扑（实测）

| 项目 | 值 |
|---|---|
| VID:PID | 1de1:f201 "USB_DISPLAY" |
| 复合设备 | HID IF0（控制）+ HID IF1（视频）+ USB Audio + Mass Storage |
| 控制端点 | EP1 OUT/IN，HID report ID 1，**4096 字节** |
| 视频端点 | EP2 OUT，HID report ID 2，**4096 字节**（注意：不是描述符里的 12288） |
| 音频 | UAC 48kHz 2ch S16_LE（ALSA plughw:USBDISPLAY,0） |

## 2. 控制通道（IF0，EP1，4096B）

报文格式（主机→设备、设备→主机相同）：

    [0]=0x01  [1..4]=BE32 JSON 字节数  [5..15]=0x00×11  [16..]=JSON(UTF-8)

* JSON-RPC 2.0；设备回复是带缩进/制表符的 pretty JSON。
* **heartbeat 必须带 session-id**，否则设备不回包：

    {"jsonrpc":"2.0", "method": "heartbeat", "params": {"session-id": "193390"}}

  设备回：

    {"jsonrpc":"2.0","method":"heartbeat","params":{"session-id":"193390"},"jrpcSeq":2}

* 实测被设备接受的方法：
  * `heartbeat`（带 session-id）
  * `app-launched` → 回 `{"id":N,"result":{"res-width":1920,"res-height":1080},"jrpcSeq":M}`
  * `app_tx_mouse_icon_change` / `app_tx_mouse_coordinate`（无回包）
  * `notify-projection-stopped` / **`notify-projection-started`** → 后者回 `{"result":"isAllowed"}`
  * `common.data_passthrough`（回复设备 EDID 请求：`{"data":"AQ==","id":103,"size":1}`）
* 实测返回 **Method not found**（本固件不支持）：`common.get_device_description`、
  `common.set_device_description`、`common.request_stream`、`common.enable_control`、
  `common.projection-request-with-tx`、`common.answer_request_stream`。
* 设备主动上报：`notify-app-accessories-are-ready`（1920x1080 fw 23099127）、
  `tx-data-updated`（role/split-count/split-position/capability）、`heartbeat`、
  `common.data_passthrough`（id=101，258 字节 = CFORCE 显示器 EDID）。

### 从抓包还原的启动序列（按原时间顺序）

    heartbeat(session-id)                          每 ~1.5s
    {"id":1,"method":"app-launched", params:{platform:"windows",
        platform-machine-name, product-type:"QuattroTx CT10",
        res-width:1920,res-height:1080}}
    {"method":"app_tx_mouse_icon_change","params":{"cursor_base64_data":""}}
    {"method":"notify-projection-stopped"}
    {"method":"common.data_passthrough","params":{"data":"AQ==","id":103,"size":1}}
    {"method":"notify-projection-stopped"}
    {"id":3,"method":"app-launched", ...}
    {"id":4,"method":"notify-projection-started"}   → {"result":"isAllowed"}
    ↓ 之后开始发视频

## 3. 视频通道（IF1，EP2，4096B）

每条 HID 报告 = 4096 字节，格式：

    第 1 个报告（每条“视频消息”的首包）：
      [0]      = 0x02
      [1..4]   = BE32 该消息 H.264 数据总长度 L
      [5..7]   = 0x00 0x00 0x00
      [8..11]  = BE32 宽（1920）
      [12..15] = BE32 高（1080）
      [16..4095] = H.264 数据前 4080 字节（补 0 到 4096）

    续包（L > 4080 时）：
      [0]=0x02，[1..15]=0x00×15，[16..4095] = 后续 4080 字节 H.264

即：一条“视频消息”= 一帧 Annex-B H.264（AUD 开头），按 4080 字节切片，
只有首包带长度/宽高，续包头全 0。

### 码流结构（从抓包逐字节解出）

* Annex-B，每个视频消息以 AUD（00 00 00 01 09 10/09 30）开头。
* 首帧消息：`AUD + SPS + PPS + SEI + IDR`。
* 后续每帧：`AUD + PPS + SEI + slice`；**每帧都带 PPS 和 SEI**。
* 抓包中 SPS 39 字节、PPS 5 字节、SEI 12/13/22 字节（buffering_period + pic_timing）。
* SPS 关键字段：Main(77) / level 4.2 / poc_type=2 / max_num_ref_frames=1 /
  log2_max_frame_num_minus4=4 / VUI: timing 1/120(60fps), fixed_frame_rate=1,
  nal_hrd=1, pic_struct=1, bitstream_restriction(max_num_reorder=0,max_dec_frame_buffering=1)。
* PPS：CABAC=1，单参考，weighted_pred=0，deblocking 控制存在。
* 全片只有开头一个 IDR，之后全是 P 帧（无周期 IDR）。

## 4. 实测结果（关键）

| 测试 | 结果 |
|---|---|
| 用 libusb 逐字节回放抓包的 EP1+EP2 报文 | ✅ **HDMI 正常显示抓包时的桌面画面** |
| 用 x264 重编码同内容（Main 4.2 / 60fps / AUD / HRD SEI / ref=1 / bframes=0 / 无重排序） | ❌ 黑屏 |
| 进一步加强：把抓包的 **SPS/PPS/每帧 SEI/AUD/NAL 头** 全部逐字节照搬，只保留 x264 的 slice（并重写 slice 头 frame_num 4→8bit 以匹配抓包 SPS） | ❌ 仍黑屏；约 37s 后设备 USB 复位（EIO） |
| 本地 ffmpeg 解码上述重建码流 | ✅ 2729 帧零错误（码流本身完全合法） |

**结论**：设备固件虽然能解码标准 H.264，但只接受其参考硬件编码器
（从 SEI/语法特征看是 NVENC 类）产生的 **slice 数据**；仅参数集/结构一致不够。
通用 x264 码流会被固件忽略（黑屏），长时间还会触发 USB 复位。

## 5. 已交付内容

* `impl/`：C++17 + CMake 用户态实现（无内核/系统文件修改）
  * 控制通道（JSON-RPC over hidraw）实机验证：diag/json 工具可用
  * 音频：ALSA USB Audio 实机播放验证
  * 视频：libusb 精确 4096B EP2 发送器 + 完整握手（配合设备兼容码流可上屏；
    Windows 抓包流已实测可显示）
* 抓包回放脚本：`work/hw/live_replay4.py`（实测可显示）
* 码流提取/转码/重建脚本：`work/hw/extract_replay.py`、`work/hw/rebuild_h264.py`、
  `work/hw/build_exact.py`、`work/hw/live_exact_long.py`
* 抓包还原出的 mp4：`work/hw/captured_from_pcap.mp4`（内容与抓包一致，已验证）

## 6. 建议的后续方案

1. **在带 NVIDIA GPU 的机器上用 NVENC 编码**（`h264_nvenc`，Main 4.2、ref=1、
   bframes=0、aud=1、repeat-headers，配 `-forced-idr`），预计与抓包编码器同源，可直接上屏；
2. 或提供任意一份 **Windows 上实际投屏得到的码流**（类似本次 pcap），
   驱动按“passthrough 模式”原样推送；
3. 若要支持任意 x264 输入，需要继续逆向固件解码器对 slice 的额外约束
   （工作量大，需设备侧调试手段）。


## 7. 追加实测（2026-11-16 晚）

* 只读检查了本机双系统的 Windows 安装（`Program Files (x86)/USB Display`，EZCast 内核）：
  配置 `AppData/Local/USB Display/setup.ini`，程序内含 x264 软编参数
  （ultrafast + zerolatency + main + threads=1 + ABR 8M/vbv1.2x/qp12-38/step3 + aud）。
* 抓包码流特征（Main+CABAC+HRD SEI+pic_struct）表明 Windows 实际走 **Intel 核显 QSV 硬编**。
* 在 Linux 侧从源码编译了支持 Meteor Lake 的 iHD 驱动（用户目录 `work/va2`，未改系统），
  `h264_vaapi` 硬编成功，生成 Main 4.2/ref=1/reorder=0/HRD 码流后发送——设备**仍黑屏**。
* 结论不变且更明确：固件只接受其参考编码器栈生成的码流，连“抓包 SPS/PPS/每帧 SEI + 合法 slice”
  与“Linux 核显硬编”都被拒绝；可显示的唯一已验证方式是逐字节回放 Windows 抓包。


## 8. 最终结论（已真机验证流畅）

* 视频黑屏的两个真实根因（均在我们的发送侧）：
  1. 按 AUD 分帧后拼接时**丢失 Annex-B 起始码**（00 00 00 01），设备收到非法码流；
  2. 使用 **x264 软编码码流**时设备能显示但**周期性抽动**——设备期望硬件编码器码流。
* 修复后：
  * libusb EP2 **4096B URB** + 16B 头/续包头全零 → 正确上屏；
  * **Intel VAAPI（iHD，与 Windows QSV 同源）编码的新画面 60fps/4Mbps → 流畅**（1200 帧，0 错误）；
  * Windows 抓包原始码流同样流畅。
* 最终使用方式见 [FINAL_USAGE.md](FINAL_USAGE.md)；编码封装脚本 [tools/usbdisplay_encode.sh](tools/usbdisplay_encode.sh)。


## 9. 编码格式逆向结论（最终、可移植）

设备兼容码流（x264 软编实测流畅）：
- Annex-B，AU 以 AUD 开头且保留起始码；
- H.264 **Main (77) / ≤4.2，CABAC，无 B 帧，ref=1，max_num_reorder_frames=0，
  max_dec_frame_buffering=1**；
- 1920x1080、60fps、3.5–4.5Mbps、帧大小均匀；
- HRD/SEI 非必需（QSV 抓包带 HRD/pic_timing，x264 不带也可流畅）。

不满足时：起始码缺失 = 黑屏；带 B 帧 = 显示但周期性抽动。
x264 参考参数：`aud=1:repeat-headers=1:bframes=0:ref=1:keyint=120:scenecut=0`。
封装脚本 [tools/usbdisplay_encode.sh](tools/usbdisplay_encode.sh)（默认 x264，可 --vaapi）。
