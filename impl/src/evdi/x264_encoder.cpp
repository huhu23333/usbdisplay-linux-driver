// SPDX-License-Identifier: MIT
#include "evdi/x264_encoder.h"

#include <libavutil/pixfmt.h>
#include <x264.h>

extern "C" {
#include <libswscale/swscale.h>
}

#include "evdi/h264_au.h"

namespace usbdisplay {
namespace evdi {

struct X264Encoder::Impl {
  x264_t* encoder = nullptr;
  SwsContext* scaler = nullptr;
  x264_picture_t picture{};
  int width = 0;
  int height = 0;
  int64_t frame_index = 0;
  bool picture_allocated = false;
};

X264Encoder::X264Encoder() : impl_(new Impl()) {}
X264Encoder::~X264Encoder() { close(); }

bool X264Encoder::open(int width, int height, int fps, int bitrate_bps,
                       std::string* err) {
  close();
  if (width <= 0 || height <= 0 || fps <= 0 || bitrate_bps <= 0) {
    if (err) *err = "x264 编码参数无效";
    return false;
  }

  x264_param_t param;
  if (x264_param_default_preset(&param, "superfast", "zerolatency") < 0) {
    if (err) *err = "x264 预设初始化失败";
    return false;
  }
  param.i_width = width;
  param.i_height = height;
  param.i_fps_num = fps;
  param.i_fps_den = 1;
  param.i_csp = X264_CSP_I420;
  param.b_vfr_input = 0;
  param.i_bframe = 0;
  param.i_frame_reference = 1;
  param.i_keyint_max = 120;
  param.i_keyint_min = 120;
  param.i_scenecut_threshold = 0;
  param.b_intra_refresh = 0;
  param.rc.i_rc_method = X264_RC_ABR;
  param.rc.i_bitrate = bitrate_bps / 1000;
  param.rc.i_vbv_max_bitrate = 4200;
  param.rc.i_vbv_buffer_size = 4000;
  param.b_aud = 1;
  param.b_repeat_headers = 1;
  param.b_annexb = 1;
  if (x264_param_apply_profile(&param, "main") < 0) {
    if (err) *err = "x264 main profile 设置失败";
    return false;
  }
  param.i_level_idc = 42;  // level 4.2

  impl_->encoder = x264_encoder_open(&param);
  if (impl_->encoder == nullptr) {
    if (err) *err = "x264_encoder_open 失败";
    close();
    return false;
  }

  impl_->scaler = sws_getContext(width, height, AV_PIX_FMT_BGR0, width, height,
                                 AV_PIX_FMT_YUV420P, SWS_FAST_BILINEAR,
                                 nullptr, nullptr, nullptr);
  if (impl_->scaler == nullptr) {
    if (err) *err = "libswscale 初始化失败";
    close();
    return false;
  }
  x264_picture_alloc(&impl_->picture, X264_CSP_I420, width, height);
  impl_->picture_allocated = true;
  impl_->width = width;
  impl_->height = height;
  impl_->frame_index = 0;
  return true;
}

bool X264Encoder::encode(const uint8_t* xrgb, int stride, uint64_t pts_us,
                         EncodedAccessUnit* out, std::string* err) {
  (void)pts_us;
  if (impl_ == nullptr || impl_->encoder == nullptr ||
      impl_->scaler == nullptr) {
    if (err) *err = "x264 编码器未打开";
    return false;
  }
  if (out == nullptr || xrgb == nullptr || stride <= 0) {
    if (err) *err = "x264 输入帧无效";
    return false;
  }
  out->data.clear();
  out->keyframe = false;

  const uint8_t* source[4] = {xrgb, nullptr, nullptr, nullptr};
  const int source_stride[4] = {stride, 0, 0, 0};
  uint8_t* destination[4] = {impl_->picture.img.plane[0],
                             impl_->picture.img.plane[1],
                             impl_->picture.img.plane[2], nullptr};
  const int destination_stride[4] = {impl_->picture.img.i_stride[0],
                                     impl_->picture.img.i_stride[1],
                                     impl_->picture.img.i_stride[2], 0};
  sws_scale(impl_->scaler, source, source_stride, 0, impl_->height,
            destination, destination_stride);

  impl_->picture.i_pts = impl_->frame_index++;
  x264_picture_t picture_out;
  x264_nal_t* nals = nullptr;
  int nal_count = 0;
  if (x264_encoder_encode(impl_->encoder, &nals, &nal_count,
                          &impl_->picture, &picture_out) < 0) {
    if (err) *err = "x264_encoder_encode 失败";
    return false;
  }
  if (nal_count <= 0) return true;

  std::vector<H264NalView> views;
  views.reserve(static_cast<size_t>(nal_count));
  for (int i = 0; i < nal_count; ++i) {
    views.push_back(
        {nals[i].p_payload, static_cast<size_t>(nals[i].i_payload)});
  }
  H264AccessUnit unit;
  std::string build_err;
  if (!build_access_unit(views, picture_out.b_keyframe != 0, &unit,
                         &build_err)) {
    if (err) *err = build_err;
    return false;
  }
  out->data = std::move(unit.data);
  out->keyframe = unit.keyframe;
  return true;
}

void X264Encoder::close() {
  if (impl_ == nullptr) return;
  if (impl_->picture_allocated) {
    x264_picture_clean(&impl_->picture);
    impl_->picture_allocated = false;
  }
  if (impl_->scaler != nullptr) {
    sws_freeContext(impl_->scaler);
    impl_->scaler = nullptr;
  }
  if (impl_->encoder != nullptr) {
    x264_encoder_close(impl_->encoder);
    impl_->encoder = nullptr;
  }
  impl_->width = 0;
  impl_->height = 0;
  impl_->frame_index = 0;
}

bool X264Encoder::is_open() const {
  return impl_ != nullptr && impl_->encoder != nullptr;
}

}  // namespace evdi
}  // namespace usbdisplay
