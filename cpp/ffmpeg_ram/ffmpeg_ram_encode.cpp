// https://github.com/FFmpeg/FFmpeg/blob/master/doc/examples/encode_video.c

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/imgutils.h>
#include <libavutil/log.h>
#include <libavutil/opt.h>
}

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define LOG_MODULE "FFMPEG_RAM_ENC"
#include <log.h>
#include <uitl.h>
#ifdef _WIN32
#include "win.h"
#endif

static int calculate_offset_length(int pix_fmt, int height, const int *linesize,
                                   int *offset, int *length) {
  switch (pix_fmt) {
  case AV_PIX_FMT_YUV420P:
    offset[0] = linesize[0] * height;
    offset[1] = offset[0] + linesize[1] * height / 2;
    *length = offset[1] + linesize[2] * height / 2;
    break;
  case AV_PIX_FMT_NV12:
    offset[0] = linesize[0] * height;
    *length = offset[0] + linesize[1] * height / 2;
    break;
  default:
    LOG_ERROR("unsupported pixfmt" + std::to_string(pix_fmt));
    return -1;
  }

  return 0;
}

extern "C" int ffmpeg_ram_get_linesize_offset_length(int pix_fmt, int width,
                                                     int height, int align,
                                                     int *linesize, int *offset,
                                                     int *length) {
  AVFrame *frame = NULL;
  int ioffset[AV_NUM_DATA_POINTERS] = {0};
  int ilength = 0;
  int ret = -1;

  if (!(frame = av_frame_alloc())) {
    LOG_ERROR("Alloc frame failed");
    goto _exit;
  }

  frame->format = pix_fmt;
  frame->width = width;
  frame->height = height;

  if ((ret = av_frame_get_buffer(frame, align)) < 0) {
    LOG_ERROR("av_frame_get_buffer, ret = " + av_err2str(ret));
    goto _exit;
  }
  if (linesize) {
    for (int i = 0; i < AV_NUM_DATA_POINTERS; i++)
      linesize[i] = frame->linesize[i];
  }
  if (offset || length) {
    ret = calculate_offset_length(pix_fmt, height, frame->linesize, ioffset,
                                  &ilength);
    if (ret < 0)
      goto _exit;
  }
  if (offset) {
    for (int i = 0; i < AV_NUM_DATA_POINTERS; i++) {
      if (ioffset[i] == 0)
        break;
      offset[i] = ioffset[i];
    }
  }
  if (length)
    *length = ilength;

  ret = 0;
_exit:
  if (frame)
    av_frame_free(&frame);
  return ret;
}

namespace {
typedef void (*RamEncodeCallback)(const uint8_t *data, int len, int64_t pts,
                                  int key, const void *obj);

class FFmpegRamEncoder {
public:
  AVCodecContext *c_ = NULL;
  AVFrame *frame_ = NULL;
  AVPacket *pkt_ = NULL;
  std::string name_;
  int64_t first_ms_ = 0;

  int width_ = 0;
  int height_ = 0;
  AVPixelFormat pixfmt_ = AV_PIX_FMT_NV12;
  int align_ = 0;
  int bit_rate_ = 0;
  int time_base_num_ = 1;
  int time_base_den_ = 30;
  int gop_ = 0xFFFF;
  int quality_ = 0;
  int rc_ = 0;
  int thread_count_ = 1;
  int gpu_ = 0;
  RamEncodeCallback callback_ = NULL;
  int offset_[AV_NUM_DATA_POINTERS] = {0};

  AVHWDeviceType hw_device_type_ = AV_HWDEVICE_TYPE_NONE;
  AVPixelFormat hw_pixfmt_ = AV_PIX_FMT_NONE;
  AVBufferRef *hw_device_ctx_ = NULL;
  AVFrame *hw_frame_ = NULL;

  FFmpegRamEncoder(const char *name, int width, int height, int pixfmt,
                   int align, int bit_rate, int time_base_num,
                   int time_base_den, int gop, int quality, int rc,
                   int thread_count, int gpu, RamEncodeCallback callback) {
    name_ = name;
    width_ = width;
    height_ = height;
    pixfmt_ = (AVPixelFormat)pixfmt;
    align_ = align;
    bit_rate_ = bit_rate;
    time_base_num_ = time_base_num;
    time_base_den_ = time_base_den;
    gop_ = gop;
    quality_ = quality;
    rc_ = rc;
    thread_count_ = thread_count;
    gpu_ = gpu;
    callback_ = callback;
    if (name_.find("vaapi") != std::string::npos) {
      hw_device_type_ = AV_HWDEVICE_TYPE_VAAPI;
      hw_pixfmt_ = AV_PIX_FMT_VAAPI;
    } else if (name_.find("nvenc") != std::string::npos) {
#ifdef _WIN32
      hw_device_type_ = AV_HWDEVICE_TYPE_D3D11VA;
      hw_pixfmt_ = AV_PIX_FMT_D3D11;
#endif
    }
  }

  ~FFmpegRamEncoder() {}

  bool init(int *linesize, int *offset, int *length) {
    const AVCodec *codec = NULL;

    int ret;

    if (!(codec = avcodec_find_encoder_by_name(name_.c_str()))) {
      LOG_ERROR("Codec " + name_ + " not found");
      return false;
    }

    if (!(c_ = avcodec_alloc_context3(codec))) {
      LOG_ERROR("Could not allocate video codec context");
      return false;
    }

    if (hw_device_type_ != AV_HWDEVICE_TYPE_NONE) {
      std::string device = "";
#ifdef _WIN32
      if (name_.find("nvenc") != std::string::npos) {
        int index = Adapters::GetFirstAdapterIndex(
            AdapterVendor::ADAPTER_VENDOR_NVIDIA);
        if (index >= 0) {
          device = std::to_string(index);
        }
      }
#endif
      ret = av_hwdevice_ctx_create(&hw_device_ctx_, hw_device_type_,
                                   device.length() == 0 ? NULL : device.c_str(),
                                   NULL, 0);
      if (ret < 0) {
        LOG_ERROR("av_hwdevice_ctx_create failed");
        return false;
      }
      if (set_hwframe_ctx() != 0) {
        LOG_ERROR("set_hwframe_ctx failed");
        return false;
      }
      hw_frame_ = av_frame_alloc();
      if (!hw_frame_) {
        LOG_ERROR("av_frame_alloc failed");
        return false;
      }
      if ((ret = av_hwframe_get_buffer(c_->hw_frames_ctx, hw_frame_, 0)) < 0) {
        LOG_ERROR("av_hwframe_get_buffer failed, ret = " + av_err2str(ret));
        return false;
      }
      if (!hw_frame_->hw_frames_ctx) {
        LOG_ERROR("hw_frame_->hw_frames_ctx is NULL");
        return false;
      }
    }

    if (!(frame_ = av_frame_alloc())) {
      LOG_ERROR("Could not allocate video frame");
      return false;
    }
    frame_->format = pixfmt_;
    frame_->width = width_;
    frame_->height = height_;

    if ((ret = av_frame_get_buffer(frame_, align_)) < 0) {
      LOG_ERROR("av_frame_get_buffer failed, ret = " + av_err2str(ret));
      return false;
    }

    if (!(pkt_ = av_packet_alloc())) {
      LOG_ERROR("Could not allocate video packet");
      return false;
    }

    /* resolution must be a multiple of two */
    c_->width = width_;
    c_->height = height_;
    c_->pix_fmt =
        hw_pixfmt_ != AV_PIX_FMT_NONE ? hw_pixfmt_ : (AVPixelFormat)pixfmt_;
    c_->sw_pix_fmt = (AVPixelFormat)pixfmt_;
    c_->has_b_frames = 0;
    c_->max_b_frames = 0;
    c_->gop_size = gop_;
    /* put sample parameters */
    // https://github.com/FFmpeg/FFmpeg/blob/415f012359364a77e8394436f222b74a8641a3ee/libavcodec/encode.c#L581
    if (bit_rate_ >= 1000) {
      c_->bit_rate = bit_rate_;
      if (name_.find("qsv") != std::string::npos) {
        c_->rc_max_rate = bit_rate_;
      }
    }
    /* frames per second */
    c_->time_base = av_make_q(time_base_num_, time_base_den_);
    c_->framerate = av_inv_q(c_->time_base);
    c_->flags |= AV_CODEC_FLAG2_LOCAL_HEADER;
    c_->flags |= AV_CODEC_FLAG_LOW_DELAY;
    c_->thread_count = thread_count_;
    c_->thread_type = FF_THREAD_SLICE;

    // https://github.com/obsproject/obs-studio/blob/3cc7dc0e7cf8b01081dc23e432115f7efd0c8877/plugins/obs-ffmpeg/obs-ffmpeg-mux.c#L160
    c_->color_range = AVCOL_RANGE_MPEG;
    c_->colorspace = AVCOL_SPC_SMPTE170M;
    c_->color_primaries = AVCOL_PRI_SMPTE170M;
    c_->color_trc = AVCOL_TRC_SMPTE170M;

    if (!util::set_lantency_free(c_->priv_data, name_)) {
      LOG_ERROR("set_lantency_free failed, name: " + name_);
      return false;
    }
    util::set_quality(c_->priv_data, name_, quality_);
    util::set_rate_control(c_->priv_data, name_, rc_);
    util::set_gpu(c_->priv_data, name_, gpu_);
    util::force_hw(c_->priv_data, name_);
    util::set_others(c_->priv_data, name_);

    if ((ret = avcodec_open2(c_, codec, NULL)) < 0) {
      LOG_ERROR("avcodec_open2 failed, ret = " + av_err2str(ret) +
                ", name: " + name_);
      return false;
    }
    first_ms_ = 0;

    if (ffmpeg_ram_get_linesize_offset_length(pixfmt_, width_, height_, align_,
                                              NULL, offset_, length) != 0)
      return false;

    for (int i = 0; i < AV_NUM_DATA_POINTERS; i++) {
      linesize[i] = frame_->linesize[i];
      offset[i] = offset_[i];
    }
    return true;
  }

  int encode(const uint8_t *data, int length, const void *obj, uint64_t ms) {
    int ret;

    if ((ret = av_frame_make_writable(frame_)) != 0) {
      LOG_ERROR("av_frame_make_writable failed, ret = " + av_err2str(ret));
      return ret;
    }
    if ((ret = fill_frame(frame_, (uint8_t *)data, length, offset_)) != 0)
      return ret;
    AVFrame *tmp_frame;
    if (hw_device_type_ != AV_HWDEVICE_TYPE_NONE) {
      if ((ret = av_hwframe_transfer_data(hw_frame_, frame_, 0)) < 0) {
        LOG_ERROR("av_hwframe_transfer_data failed, ret = " + av_err2str(ret));
        return ret;
      }
      tmp_frame = hw_frame_;
    } else {
      tmp_frame = frame_;
    }

    return do_encode(tmp_frame, obj, ms);
  }

  void free_encoder() {
    if (pkt_)
      av_packet_free(&pkt_);
    if (frame_)
      av_frame_free(&frame_);
    if (hw_frame_)
      av_frame_free(&hw_frame_);
    if (hw_device_ctx_)
      av_buffer_unref(&hw_device_ctx_);
    if (c_)
      avcodec_free_context(&c_);
  }

  int set_bitrate(int bitrate) {
    if (name_.find("nvenc") != std::string::npos ||
        name_.find("amf") != std::string::npos) {
      c_->bit_rate = bitrate;
      return 0;
    }
    LOG_ERROR("ffmpeg_ram_set_bitrate " + name_ +
              " does not implement bitrate change");
    return -1;
  }

private:
  int set_hwframe_ctx() {
    AVBufferRef *hw_frames_ref;
    AVHWFramesContext *frames_ctx = NULL;
    int err = 0;

    if (!(hw_frames_ref = av_hwframe_ctx_alloc(hw_device_ctx_))) {
      LOG_ERROR("av_hwframe_ctx_alloc failed");
      return -1;
    }
    frames_ctx = (AVHWFramesContext *)(hw_frames_ref->data);
    frames_ctx->format = hw_pixfmt_;
    frames_ctx->sw_format = (AVPixelFormat)pixfmt_;
    frames_ctx->width = width_;
    frames_ctx->height = height_;
    frames_ctx->initial_pool_size = 1;
    if ((err = av_hwframe_ctx_init(hw_frames_ref)) < 0) {
      av_buffer_unref(&hw_frames_ref);
      return err;
    }
    c_->hw_frames_ctx = av_buffer_ref(hw_frames_ref);
    if (!c_->hw_frames_ctx) {
      LOG_ERROR("av_buffer_ref failed");
      err = -1;
    }
    av_buffer_unref(&hw_frames_ref);
    return err;
  }

  int do_encode(AVFrame *frame, const void *obj, int64_t ms) {
    int ret;
    bool encoded = false;
    if ((ret = avcodec_send_frame(c_, frame)) < 0) {
      LOG_ERROR("avcodec_send_frame failed, ret = " + av_err2str(ret));
      return ret;
    }

    while (ret >= 0) {
      if ((ret = avcodec_receive_packet(c_, pkt_)) < 0) {
        if (ret != AVERROR(EAGAIN)) {
          LOG_ERROR("avcodec_receive_packet failed, ret = " + av_err2str(ret));
        }
        goto _exit;
      }
      encoded = true;
      if (first_ms_ == 0)
        first_ms_ = ms;
      callback_(pkt_->data, pkt_->size, ms - first_ms_,
                pkt_->flags & AV_PKT_FLAG_KEY, obj);
    }
  _exit:
    av_packet_unref(pkt_);
    return encoded ? 0 : -1;
  }

  int fill_frame(AVFrame *frame, uint8_t *data, int data_length,
                 const int *const offset) {
    switch (frame->format) {
    case AV_PIX_FMT_NV12:
      if (data_length <
          frame->height * (frame->linesize[0] + frame->linesize[1] / 2)) {
        LOG_ERROR("fill_frame: NV12 data length error. data_length:" +
                  std::to_string(data_length) +
                  ", linesize[0]:" + std::to_string(frame->linesize[0]) +
                  ", linesize[1]:" + std::to_string(frame->linesize[1]));
        return -1;
      }
      frame->data[0] = data;
      frame->data[1] = data + offset[0];
      break;
    case AV_PIX_FMT_YUV420P:
      if (data_length <
          frame->height * (frame->linesize[0] + frame->linesize[1] / 2 +
                           frame->linesize[2] / 2)) {
        LOG_ERROR("fill_frame: 420P data length error. data_length:" +
                  std::to_string(data_length) +
                  ", linesize[0]:" + std::to_string(frame->linesize[0]) +
                  ", linesize[1]:" + std::to_string(frame->linesize[1]) +
                  ", linesize[2]:" + std::to_string(frame->linesize[2]));
        return -1;
      }
      frame->data[0] = data;
      frame->data[1] = data + offset[0];
      frame->data[2] = data + offset[1];
      break;
    default:
      LOG_ERROR("fill_frame: unsupported format, " +
                std::to_string(frame->format));
      return -1;
    }
    return 0;
  }
};

} // namespace

extern "C" FFmpegRamEncoder *
ffmpeg_ram_new_encoder(const char *name, int width, int height, int pixfmt,
                       int align, int bit_rate, int time_base_num,
                       int time_base_den, int gop, int quality, int rc,
                       int thread_count, int gpu, int *linesize, int *offset,
                       int *length, RamEncodeCallback callback) {
  FFmpegRamEncoder *encoder = NULL;
  try {
    encoder = new FFmpegRamEncoder(name, width, height, pixfmt, align, bit_rate,
                                   time_base_num, time_base_den, gop, quality,
                                   rc, thread_count, gpu, callback);
    if (encoder) {
      if (encoder->init(linesize, offset, length)) {
        return encoder;
      }
    }
  } catch (const std::exception &e) {
    LOG_ERROR("new FFmpegRamEncoder failed, " + std::string(e.what()));
  }
  if (encoder) {
    encoder->free_encoder();
    delete encoder;
    encoder = NULL;
  }
  return NULL;
}

extern "C" int ffmpeg_ram_encode(FFmpegRamEncoder *encoder, const uint8_t *data,
                                 int length, const void *obj, uint64_t ms) {
  try {
    return encoder->encode(data, length, obj, ms);
  } catch (const std::exception &e) {
    LOG_ERROR("ffmpeg_ram_encode failed, " + std::string(e.what()));
  }
  return -1;
}

extern "C" void ffmpeg_ram_free_encoder(FFmpegRamEncoder *encoder) {
  try {
    if (!encoder)
      return;
    encoder->free_encoder();
    delete encoder;
    encoder = NULL;
  } catch (const std::exception &e) {
    LOG_ERROR("free encoder failed, " + std::string(e.what()));
  }
}

extern "C" int ffmpeg_ram_set_bitrate(FFmpegRamEncoder *encoder, int bitrate) {
  try {
    return encoder->set_bitrate(bitrate);
  } catch (const std::exception &e) {
    LOG_ERROR("ffmpeg_ram_set_bitrate failed, " + std::string(e.what()));
  }
  return -1;
}