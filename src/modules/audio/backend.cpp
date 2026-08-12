#include <alsa/asoundlib.h>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "rkavp/backends/alsa.hpp"

namespace rkavp {
namespace {

snd_pcm_format_t AlsaFormat(const std::string& format) {
  if (format == "s16_le") return SND_PCM_FORMAT_S16_LE;
  if (format == "s24_le") return SND_PCM_FORMAT_S24_LE;
  if (format == "s32_le") return SND_PCM_FORMAT_S32_LE;
  if (format == "float32_le") return SND_PCM_FORMAT_FLOAT_LE;
  if (format == "u8") return SND_PCM_FORMAT_U8;
  return SND_PCM_FORMAT_UNKNOWN;
}

class AlsaBackend final : public IAlsaBackend {
 public:
  ~AlsaBackend() override { Close(); }

  Status Open(const AlsaCaptureConfig& config) override {
    Close();
    if (config.sample_rate <= 0 || config.channels <= 0 || config.frame_ms <= 0) {
      return Status::Invalid("invalid ALSA capture configuration");
    }
    format_ = AlsaFormat(config.format);
    if (format_ == SND_PCM_FORMAT_UNKNOWN)
      return Status::Invalid("unsupported ALSA format: " + config.format);

    snd_pcm_t* pcm = nullptr;
    int result = snd_pcm_open(&pcm, config.device.c_str(), SND_PCM_STREAM_CAPTURE, 0);
    if (result < 0)
      return Status::Unavailable(std::string("snd_pcm_open failed: ") + snd_strerror(result));

    snd_pcm_hw_params_t* hardware = nullptr;
    snd_pcm_hw_params_alloca(&hardware);
    result = snd_pcm_hw_params_any(pcm, hardware);
    if (result >= 0)
      result = snd_pcm_hw_params_set_access(pcm, hardware, SND_PCM_ACCESS_RW_INTERLEAVED);
    if (result >= 0) result = snd_pcm_hw_params_set_format(pcm, hardware, format_);
    unsigned int rate = static_cast<unsigned int>(config.sample_rate);
    if (result >= 0) result = snd_pcm_hw_params_set_rate_near(pcm, hardware, &rate, nullptr);
    if (result >= 0)
      result =
          snd_pcm_hw_params_set_channels(pcm, hardware, static_cast<unsigned int>(config.channels));
    snd_pcm_uframes_t period = std::max<snd_pcm_uframes_t>(1, rate * config.frame_ms / 1000);
    if (result >= 0)
      result = snd_pcm_hw_params_set_period_size_near(pcm, hardware, &period, nullptr);
    if (result >= 0) result = snd_pcm_hw_params(pcm, hardware);
    if (result < 0) {
      snd_pcm_close(pcm);
      return Status::Unavailable(std::string("ALSA hardware negotiation failed: ") +
                                 snd_strerror(result));
    }

    snd_pcm_sw_params_t* software = nullptr;
    snd_pcm_sw_params_alloca(&software);
    result = snd_pcm_sw_params_current(pcm, software);
    if (result >= 0)
      result = snd_pcm_sw_params_set_tstamp_mode(pcm, software, SND_PCM_TSTAMP_ENABLE);
#ifdef SND_PCM_TSTAMP_TYPE_MONOTONIC
    if (result >= 0)
      result = snd_pcm_sw_params_set_tstamp_type(pcm, software, SND_PCM_TSTAMP_TYPE_MONOTONIC);
#endif
    if (result >= 0) result = snd_pcm_sw_params(pcm, software);
    if (result >= 0) result = snd_pcm_prepare(pcm);
    if (result < 0) {
      snd_pcm_close(pcm);
      return Status::Unavailable(std::string("ALSA prepare failed: ") + snd_strerror(result));
    }

    std::lock_guard<std::mutex> lock(mutex_);
    pcm_ = pcm;
    config_ = config;
    config_.sample_rate = static_cast<int>(rate);
    frames_per_read_ = period;
    bytes_per_sample_ = snd_pcm_format_physical_width(format_) / 8;
    stop_ = false;
    return Status::Ok();
  }

  Status Read(AudioFrame* frame) override {
    if (frame == nullptr) return Status::Invalid("ALSA output frame is null");
    snd_pcm_t* pcm = nullptr;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      pcm = pcm_;
    }
    if (pcm == nullptr) return Status::FailedPrecondition("ALSA capture is not open");
    if (stop_) return Status::Cancelled("ALSA capture stopped");

    const int wait_result = snd_pcm_wait(pcm, 100);
    if (stop_) return Status::Cancelled("ALSA capture stopped");
    if (wait_result == 0) return Status::Unavailable("ALSA capture timeout");
    if (wait_result < 0)
      return Status::Unavailable(std::string("ALSA wait failed: ") + snd_strerror(wait_result));

    const std::size_t byte_count =
        static_cast<std::size_t>(frames_per_read_) * config_.channels * bytes_per_sample_;
    std::vector<std::uint8_t> bytes(byte_count);
    const snd_pcm_sframes_t captured = snd_pcm_readi(pcm, bytes.data(), frames_per_read_);
    if (captured == -EPIPE || captured == -ESTRPIPE) {
      return Status::Internal("ALSA capture overrun");
    }
    if (captured == -EAGAIN) return Status::Unavailable("ALSA capture would block");
    if (captured < 0)
      return Status::Unavailable(std::string("ALSA read failed: ") +
                                 snd_strerror(static_cast<int>(captured)));
    bytes.resize(static_cast<std::size_t>(captured) * config_.channels * bytes_per_sample_);

    snd_pcm_status_t* status = nullptr;
    snd_pcm_status_alloca(&status);
    snd_htimestamp_t timestamp{};
    if (snd_pcm_status(pcm, status) >= 0) snd_pcm_status_get_htstamp(status, &timestamp);
    const std::int64_t timestamp_us = static_cast<std::int64_t>(timestamp.tv_sec) * 1000000LL +
                                      static_cast<std::int64_t>(timestamp.tv_nsec) / 1000LL;
    frame->pts = Timestamp::FromMicroseconds(timestamp_us);
    frame->sample_rate = config_.sample_rate;
    frame->channels = config_.channels;
    frame->samples_per_channel = static_cast<int>(captured);
    frame->format = config_.format;
    frame->buffer = std::make_shared<HostBuffer>(std::move(bytes));
    return Status::Ok();
  }

  Status RecoverOverrun() override {
    std::lock_guard<std::mutex> lock(mutex_);
    if (pcm_ == nullptr) return Status::FailedPrecondition("ALSA capture is not open");
    const int result = snd_pcm_prepare(pcm_);
    return result >= 0 ? Status::Ok()
                       : Status::Unavailable(std::string("ALSA overrun recovery failed: ") +
                                             snd_strerror(result));
  }

  void RequestStop() override {
    stop_ = true;
    std::lock_guard<std::mutex> lock(mutex_);
    if (pcm_ != nullptr) snd_pcm_abort(pcm_);
  }

  void Close() override {
    RequestStop();
    std::lock_guard<std::mutex> lock(mutex_);
    if (pcm_ != nullptr) snd_pcm_close(pcm_);
    pcm_ = nullptr;
  }

 private:
  std::mutex mutex_;
  snd_pcm_t* pcm_ = nullptr;
  AlsaCaptureConfig config_;
  snd_pcm_format_t format_ = SND_PCM_FORMAT_UNKNOWN;
  snd_pcm_uframes_t frames_per_read_ = 0;
  int bytes_per_sample_ = 0;
  std::atomic<bool> stop_{true};
};

}  // namespace

std::unique_ptr<IAlsaBackend> CreateAlsaBackend() { return std::make_unique<AlsaBackend>(); }

}  // namespace rkavp
