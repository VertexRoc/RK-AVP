#include <mpp_buffer.h>
#include <mpp_frame.h>
#include <mpp_packet.h>
#include <rk_mpi.h>
#include <rk_venc_cfg.h>

#include <algorithm>
#include <cstring>
#include <vector>

#include "../backend_factory.hpp"

namespace rkavp {
namespace {

MppCodingType CodingType(const std::string& codec) {
  if (codec == "h264" || codec == "avc") return MPP_VIDEO_CodingAVC;
  if (codec == "h265" || codec == "hevc") return MPP_VIDEO_CodingHEVC;
  if (codec == "mjpeg" || codec == "jpeg") return MPP_VIDEO_CodingMJPEG;
  return MPP_VIDEO_CodingUnused;
}

MppFrameFormat FrameFormat(const std::string& format) {
  if (format == "nv12") return MPP_FMT_YUV420SP;
  if (format == "nv21") return MPP_FMT_YUV420SP_VU;
  if (format == "yuyv") return MPP_FMT_YUV422_YUYV;
  if (format == "rgb888") return MPP_FMT_RGB888;
  if (format == "rgba8888") return MPP_FMT_RGBA8888;
  return MPP_FMT_YUV420SP;
}

Status RateControlMode(const std::string& name, MppEncRcMode* mode) {
  if (mode == nullptr) return Status::Invalid("MPP rate-control output is null");
  if (name == "cbr")
    *mode = MPP_ENC_RC_MODE_CBR;
  else if (name == "vbr")
    *mode = MPP_ENC_RC_MODE_VBR;
  else if (name == "fixqp")
    *mode = MPP_ENC_RC_MODE_FIXQP;
  else if (name == "avbr")
    *mode = MPP_ENC_RC_MODE_AVBR;
  else if (name == "smtrc")
    *mode = MPP_ENC_RC_MODE_SMTRC;
  else if (name == "se")
    *mode = MPP_ENC_RC_MODE_SE;
  else
    return Status::Invalid("unsupported MPP rate_control: " + name);
  return Status::Ok();
}

Status ProfileValue(MppCodingType coding, const std::string& name, int* profile) {
  if (profile == nullptr) return Status::Invalid("MPP profile output is null");
  if (name.empty()) return Status::Ok();
  if (coding == MPP_VIDEO_CodingAVC) {
    if (name == "baseline")
      *profile = 66;
    else if (name == "main")
      *profile = 77;
    else if (name == "high")
      *profile = 100;
    else
      return Status::Invalid("H.264 profile must be baseline, main, or high");
    return Status::Ok();
  }
  if (coding == MPP_VIDEO_CodingHEVC) {
    if (name != "main") return Status::Invalid("H.265 profile must be main");
    *profile = 1;
    return Status::Ok();
  }
  return Status::Invalid("profile is only supported for H.264 and H.265");
}

std::string FrameFormatName(MppFrameFormat format) {
  switch (format & MPP_FRAME_FMT_MASK) {
    case MPP_FMT_YUV420SP:
      return "nv12";
    case MPP_FMT_YUV420SP_VU:
      return "nv21";
    case MPP_FMT_YUV422_YUYV:
      return "yuyv";
    case MPP_FMT_RGB888:
      return "rgb888";
    case MPP_FMT_RGBA8888:
      return "rgba8888";
    default:
      return "unknown";
  }
}

class MppFrameBuffer final : public Buffer {
 public:
  explicit MppFrameBuffer(MppFrame frame) : frame_(frame) {
    MppBuffer buffer = mpp_frame_get_buffer(frame_);
    if (buffer != nullptr) {
      buffer_ = buffer;
      mpp_buffer_inc_ref(buffer_);
      planes_.push_back({mpp_buffer_get_fd(buffer_), 0, mpp_buffer_get_size(buffer_),
                         static_cast<std::size_t>(mpp_frame_get_hor_stride(frame_)), 1, 0});
    }
  }
  ~MppFrameBuffer() override {
    if (buffer_ != nullptr) mpp_buffer_put(buffer_);
    if (frame_ != nullptr) mpp_frame_deinit(&frame_);
  }
  MemoryType memory_type() const override { return MemoryType::kMpp; }
  std::size_t size() const override {
    return buffer_ == nullptr ? 0 : mpp_buffer_get_size(buffer_);
  }
  const std::vector<BufferPlane>& planes() const override { return planes_; }
  void* Map(MapAccess) override {
    return buffer_ == nullptr ? nullptr : mpp_buffer_get_ptr(buffer_);
  }
  MppBuffer mpp_buffer() const { return buffer_; }

 private:
  MppFrame frame_ = nullptr;
  MppBuffer buffer_ = nullptr;
  std::vector<BufferPlane> planes_;
};

class MppDecoderBackend final : public IMppDecoderBackend {
 public:
  ~MppDecoderBackend() override { Close(); }
  Status Open(const MppDecoderConfig& config) override {
    Close();
    const MppCodingType coding = CodingType(config.codec);
    if (coding == MPP_VIDEO_CodingUnused)
      return Status::Invalid("unsupported MPP decoder codec: " + config.codec);
    if (mpp_create(&context_, &api_) != MPP_OK ||
        mpp_init(context_, MPP_CTX_DEC, coding) != MPP_OK) {
      Close();
      return Status::Unavailable("MPP decoder initialization failed");
    }
    RK_U32 timeout = MPP_POLL_NON_BLOCK;
    api_->control(context_, MPP_SET_OUTPUT_TIMEOUT, &timeout);
    if (config.external_buffer_group) {
      if (mpp_buffer_group_get_external(&group_, MPP_BUFFER_TYPE_DRM) != MPP_OK) {
        Close();
        return Status::Unavailable("MPP external buffer group creation failed");
      }
      if (config.width > 0 && config.height > 0) {
        const std::size_t size = static_cast<std::size_t>(config.width) * config.height * 3 / 2;
        mpp_buffer_group_limit_config(group_, size, static_cast<RK_S32>(config.buffer_count));
      }
      api_->control(context_, MPP_DEC_SET_EXT_BUF_GROUP, group_);
    }
    config_ = config;
    return Status::Ok();
  }

  Status Submit(const EncodedPacket& packet) override {
    if (context_ == nullptr || !packet.buffer)
      return Status::FailedPrecondition("MPP decoder is not open or packet is empty");
    Status status = packet.buffer->BeginCpuAccess(MapAccess::kRead);
    if (!status.ok()) return status;
    void* data = packet.buffer->Map(MapAccess::kRead);
    if (data == nullptr) {
      packet.buffer->EndCpuAccess(MapAccess::kRead);
      return Status::Invalid("encoded packet buffer is not CPU mappable");
    }
    MppPacket mpp_packet = nullptr;
    if (mpp_packet_init(&mpp_packet, data, packet.buffer->size()) != MPP_OK) {
      packet.buffer->Unmap();
      packet.buffer->EndCpuAccess(MapAccess::kRead);
      return Status::Internal("mpp_packet_init failed");
    }
    mpp_packet_set_pts(mpp_packet, packet.pts.is_range_value() ? packet.pts.microseconds() : 0);
    const MPP_RET result = api_->decode_put_packet(context_, mpp_packet);
    mpp_packet_deinit(&mpp_packet);
    packet.buffer->Unmap();
    status = packet.buffer->EndCpuAccess(MapAccess::kRead);
    if (!status.ok()) return status;
    return result == MPP_OK ? Status::Ok() : Status::Unavailable("MPP decoder input queue is full");
  }

  Status Receive(VideoFrame* output, bool* end_of_stream) override {
    if (output == nullptr || end_of_stream == nullptr || context_ == nullptr) {
      return Status::FailedPrecondition("MPP decoder is not open or output is null");
    }
    *end_of_stream = false;
    MppFrame frame = nullptr;
    const MPP_RET result = api_->decode_get_frame(context_, &frame);
    if (result != MPP_OK || frame == nullptr)
      return Status::Unavailable("MPP decoder has no output frame");
    if (mpp_frame_get_info_change(frame)) {
      if (group_ != nullptr) {
        mpp_buffer_group_limit_config(group_, mpp_frame_get_buf_size(frame),
                                      static_cast<RK_S32>(config_.buffer_count));
      }
      api_->control(context_, MPP_DEC_SET_INFO_CHANGE_READY, nullptr);
      mpp_frame_deinit(&frame);
      return Status::Unavailable("MPP decoder applied info change");
    }
    *end_of_stream = mpp_frame_get_eos(frame) != 0;
    if (mpp_frame_get_buffer(frame) == nullptr) {
      mpp_frame_deinit(&frame);
      return Status::Ok();
    }
    output->pts = Timestamp::FromMicroseconds(mpp_frame_get_pts(frame));
    output->frame_id = sequence_++;
    output->width = static_cast<int>(mpp_frame_get_width(frame));
    output->height = static_cast<int>(mpp_frame_get_height(frame));
    output->horizontal_stride = static_cast<int>(mpp_frame_get_hor_stride(frame));
    output->vertical_stride = static_cast<int>(mpp_frame_get_ver_stride(frame));
    output->format = FrameFormatName(mpp_frame_get_fmt(frame));
    output->buffer = std::make_shared<MppFrameBuffer>(frame);
    return Status::Ok();
  }

  Status Drain() override {
    if (context_ == nullptr) return Status::FailedPrecondition("MPP decoder is not open");
    MppPacket packet = nullptr;
    if (mpp_packet_init(&packet, nullptr, 0) != MPP_OK)
      return Status::Internal("create MPP EOS packet failed");
    mpp_packet_set_eos(packet);
    const MPP_RET result = api_->decode_put_packet(context_, packet);
    mpp_packet_deinit(&packet);
    return result == MPP_OK ? Status::Ok() : Status::Unavailable("MPP decoder rejected EOS");
  }

  void Close() override {
    if (context_ != nullptr) mpp_destroy(context_);
    context_ = nullptr;
    api_ = nullptr;
    if (group_ != nullptr) mpp_buffer_group_put(group_);
    group_ = nullptr;
  }

 private:
  MppCtx context_ = nullptr;
  MppApi* api_ = nullptr;
  MppBufferGroup group_ = nullptr;
  MppDecoderConfig config_;
  std::uint64_t sequence_ = 0;
};

class MppEncoderBackend final : public IMppEncoderBackend {
 public:
  ~MppEncoderBackend() override { Close(); }
  Status Open(const MppEncoderConfig& config) override {
    Close();
    const MppCodingType coding = CodingType(config.codec);
    if (coding == MPP_VIDEO_CodingUnused || config.width <= 0 || config.height <= 0 ||
        config.fps <= 0) {
      return Status::Invalid("invalid MPP encoder configuration");
    }
    MppEncRcMode rate_control = MPP_ENC_RC_MODE_CBR;
    Status validation = RateControlMode(config.rate_control, &rate_control);
    int profile = 0;
    if (validation.ok()) validation = ProfileValue(coding, config.profile, &profile);
    if (!validation.ok()) return validation;
    if (mpp_create(&context_, &api_) != MPP_OK ||
        mpp_init(context_, MPP_CTX_ENC, coding) != MPP_OK) {
      Close();
      return Status::Unavailable("MPP encoder initialization failed");
    }
    MppEncCfg encoder_config = nullptr;
    if (mpp_enc_cfg_init(&encoder_config) != MPP_OK)
      return Status::Internal("mpp_enc_cfg_init failed");
    mpp_enc_cfg_set_s32(encoder_config, "prep:width", config.width);
    mpp_enc_cfg_set_s32(encoder_config, "prep:height", config.height);
    mpp_enc_cfg_set_s32(encoder_config, "prep:hor_stride", config.width);
    mpp_enc_cfg_set_s32(encoder_config, "prep:ver_stride", config.height);
    mpp_enc_cfg_set_s32(encoder_config, "prep:format", MPP_FMT_YUV420SP);
    mpp_enc_cfg_set_s32(encoder_config, "rc:fps_in_num", config.fps);
    mpp_enc_cfg_set_s32(encoder_config, "rc:fps_in_denom", 1);
    mpp_enc_cfg_set_s32(encoder_config, "rc:fps_out_num", config.fps);
    mpp_enc_cfg_set_s32(encoder_config, "rc:fps_out_denom", 1);
    mpp_enc_cfg_set_s32(encoder_config, "rc:mode", rate_control);
    mpp_enc_cfg_set_s32(encoder_config, "rc:gop", config.gop > 0 ? config.gop : config.fps * 2);
    mpp_enc_cfg_set_s32(encoder_config, "rc:bps_target", config.bitrate);
    mpp_enc_cfg_set_s32(encoder_config, "rc:bps_min", config.bitrate * 8 / 10);
    mpp_enc_cfg_set_s32(encoder_config, "rc:bps_max", config.bitrate * 12 / 10);
    mpp_enc_cfg_set_s32(encoder_config, "codec:type", coding);
    if (!config.profile.empty() && coding == MPP_VIDEO_CodingAVC) {
      mpp_enc_cfg_set_s32(encoder_config, "h264:profile", profile);
    } else if (!config.profile.empty() && coding == MPP_VIDEO_CodingHEVC) {
      mpp_enc_cfg_set_s32(encoder_config, "h265:profile", profile);
    }
    const MPP_RET result = api_->control(context_, MPP_ENC_SET_CFG, encoder_config);
    mpp_enc_cfg_deinit(encoder_config);
    if (result != MPP_OK) {
      Close();
      return Status::Unavailable("MPP encoder configuration failed");
    }
    config_ = config;
    return Status::Ok();
  }

  Status Submit(const VideoFrame& input) override {
    if (context_ == nullptr || !input.buffer)
      return Status::FailedPrecondition("MPP encoder is not open or frame is empty");
    MppFrame frame = nullptr;
    if (mpp_frame_init(&frame) != MPP_OK) return Status::Internal("mpp_frame_init failed");
    mpp_frame_set_width(frame, input.width);
    mpp_frame_set_height(frame, input.height);
    mpp_frame_set_hor_stride(frame,
                             input.horizontal_stride > 0 ? input.horizontal_stride : input.width);
    mpp_frame_set_ver_stride(frame,
                             input.vertical_stride > 0 ? input.vertical_stride : input.height);
    mpp_frame_set_fmt(frame, FrameFormat(input.format));
    mpp_frame_set_pts(frame, input.pts.is_range_value() ? input.pts.microseconds() : 0);
    MppBuffer imported = nullptr;
    if (auto* mpp = dynamic_cast<MppFrameBuffer*>(input.buffer.get())) {
      mpp_frame_set_buffer(frame, mpp->mpp_buffer());
    } else if (!input.buffer->planes().empty() && input.buffer->planes().front().fd >= 0) {
      MppBufferInfo info{};
      info.type = MPP_BUFFER_TYPE_DRM;
      info.fd = input.buffer->planes().front().fd;
      info.size = input.buffer->size();
      if (mpp_buffer_import(&imported, &info) != MPP_OK) {
        mpp_frame_deinit(&frame);
        return Status::Unavailable("MPP import DMA-BUF failed");
      }
      mpp_frame_set_buffer(frame, imported);
    } else {
      mpp_frame_deinit(&frame);
      return Status::Invalid("MPP encoder requires MPP or DMA-BUF input");
    }
    const MPP_RET result = api_->encode_put_frame(context_, frame);
    mpp_frame_deinit(&frame);
    if (imported != nullptr) mpp_buffer_put(imported);
    return result == MPP_OK ? Status::Ok() : Status::Unavailable("MPP encoder rejected frame");
  }

  Status Receive(EncodedPacket* output, bool* end_of_stream) override {
    if (output == nullptr || end_of_stream == nullptr || context_ == nullptr) {
      return Status::FailedPrecondition("MPP encoder is not open or output is null");
    }
    *end_of_stream = false;
    MppPacket packet = nullptr;
    const MPP_RET result = api_->encode_get_packet(context_, &packet);
    if (result != MPP_OK || packet == nullptr)
      return Status::Unavailable("MPP encoder has no output packet");
    const std::size_t length = mpp_packet_get_length(packet);
    *end_of_stream = mpp_packet_get_eos(packet) != 0;
    std::vector<std::uint8_t> bytes(length);
    if (length != 0) std::memcpy(bytes.data(), mpp_packet_get_pos(packet), length);
    output->pts = Timestamp::FromMicroseconds(mpp_packet_get_pts(packet));
    output->dts = output->pts;
    output->codec = config_.codec;
    output->buffer = std::make_shared<HostBuffer>(std::move(bytes));
    mpp_packet_deinit(&packet);
    return Status::Ok();
  }

  Status Drain() override {
    if (context_ == nullptr) return Status::FailedPrecondition("MPP encoder is not open");
    MppFrame frame = nullptr;
    mpp_frame_init(&frame);
    mpp_frame_set_eos(frame, 1);
    const MPP_RET result = api_->encode_put_frame(context_, frame);
    mpp_frame_deinit(&frame);
    return result == MPP_OK ? Status::Ok() : Status::Unavailable("MPP encoder rejected EOS");
  }

  Status ForceKeyFrame() override {
    return context_ != nullptr && api_->control(context_, MPP_ENC_SET_IDR_FRAME, nullptr) == MPP_OK
               ? Status::Ok()
               : Status::Unavailable("MPP force key frame failed");
  }

  void Close() override {
    if (context_ != nullptr) mpp_destroy(context_);
    context_ = nullptr;
    api_ = nullptr;
  }

 private:
  MppCtx context_ = nullptr;
  MppApi* api_ = nullptr;
  MppEncoderConfig config_;
};

}  // namespace

std::unique_ptr<IMppDecoderBackend> CreateRockchipMppDecoder() {
  return std::make_unique<MppDecoderBackend>();
}
std::unique_ptr<IMppEncoderBackend> CreateRockchipMppEncoder() {
  return std::make_unique<MppEncoderBackend>();
}

}  // namespace rkavp
