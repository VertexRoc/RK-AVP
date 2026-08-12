#include <mk_mediakit.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "rkavp/backends/streaming.hpp"

namespace rkavp {
namespace {

void EnsureEnvironment() {
  static std::once_flag initialized;
  std::call_once(initialized, [] {
    mk_config config{};
    config.thread_num = 0;
    config.log_level = 2;
    config.log_mask = 0;
    mk_env_init(&config);
  });
}

int CodecId(const std::string& codec) {
  if (codec == "h264" || codec == "avc") return MKCodecH264;
  if (codec == "h265" || codec == "hevc") return MKCodecH265;
  if (codec == "mjpeg" || codec == "jpeg") return MKCodecJPEG;
  return -1;
}

class ZlmInputBackend final : public IStreamingInputBackend {
 public:
  ~ZlmInputBackend() override { Close(); }

  Status Open(const StreamingSessionConfig& config) override {
    Close();
    if (config.url.empty()) return Status::Invalid("streaming input URL is empty");
    EnsureEnvironment();
    config_ = config;
    {
      std::lock_guard<std::mutex> player_lock(player_mutex_);
      player_ = mk_player_create();
      if (player_ == nullptr) return Status::Unavailable("mk_player_create failed");
      mk_player_set_option(player_, "rtp_type", config.transport == "udp" ? "0" : "1");
      mk_player_set_on_result(player_, &ZlmInputBackend::OnPlay, this);
      mk_player_set_on_shutdown(player_, &ZlmInputBackend::OnShutdown, this);
    }
    {
      std::lock_guard<std::mutex> lock(mutex_);
      stopped_ = false;
      attempts_ = 0;
    }
    {
      std::lock_guard<std::mutex> player_lock(player_mutex_);
      if (player_ != nullptr) mk_player_play(player_, config_.url.c_str());
    }
    return Status::Ok();
  }

  Status Read(EncodedPacket* packet) override {
    if (packet == nullptr) return Status::Invalid("streaming input output is null");
    std::unique_lock<std::mutex> lock(mutex_);
    condition_.wait_for(lock, std::chrono::milliseconds(100),
                        [this] { return stopped_ || !packets_.empty() || reconnect_required_; });
    if (stopped_) return Status::Cancelled("streaming input stopped");
    if (!packets_.empty()) {
      *packet = std::move(packets_.front());
      packets_.pop_front();
      return Status::Ok();
    }
    if (reconnect_required_) {
      if (config_.reconnect.max_attempts >= 0 && attempts_ >= config_.reconnect.max_attempts) {
        return Status::Unavailable("streaming input reconnect attempts exhausted");
      }
      reconnect_required_ = false;
      const int attempt = attempts_++;
      const double multiplier = std::max(1.0, config_.reconnect.multiplier);
      const double delay = config_.reconnect.initial_delay_ms * std::pow(multiplier, attempt);
      const int delay_ms = std::min(config_.reconnect.maximum_delay_ms, static_cast<int>(delay));
      if (condition_.wait_for(lock, std::chrono::milliseconds(delay_ms),
                              [this] { return stopped_.load(); })) {
        return Status::Cancelled("streaming input stopped");
      }
      const std::string url = config_.url;
      lock.unlock();
      {
        std::lock_guard<std::mutex> player_lock(player_mutex_);
        if (player_ != nullptr) mk_player_play(player_, url.c_str());
      }
      return Status::Unavailable("streaming input reconnecting");
    }
    return Status::Unavailable("streaming input has no packet");
  }

  void RequestStop() override {
    stopped_ = true;
    condition_.notify_all();
  }

  void Close() override {
    RequestStop();
    std::vector<Delegate> delegates;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      delegates.swap(delegates_);
      packets_.clear();
      reconnect_required_ = false;
    }
    std::lock_guard<std::mutex> player_lock(player_mutex_);
    if (player_ != nullptr) {
      mk_player_set_on_result(player_, nullptr, nullptr);
      mk_player_set_on_shutdown(player_, nullptr, nullptr);
      for (const auto& delegate : delegates) mk_track_del_delegate(delegate.track, delegate.tag);
      mk_player_release(player_);
      player_ = nullptr;
    }
  }

 private:
  struct Delegate {
    mk_track track = nullptr;
    void* tag = nullptr;
  };

  static void OnPlay(void* user, int error, const char*, mk_track tracks[], int count) {
    auto* self = static_cast<ZlmInputBackend*>(user);
    if (error != 0) {
      std::lock_guard<std::mutex> lock(self->mutex_);
      self->reconnect_required_ = !self->stopped_;
      self->condition_.notify_all();
      return;
    }
    {
      std::lock_guard<std::mutex> lock(self->mutex_);
      if (self->stopped_) return;
      self->attempts_ = 0;
    }
    for (int i = 0; i < count; ++i) {
      if (!mk_track_is_video(tracks[i])) continue;
      void* tag = mk_track_add_delegate(tracks[i], &ZlmInputBackend::OnFrame, self);
      if (tag != nullptr) {
        bool remove_delegate = false;
        {
          std::lock_guard<std::mutex> lock(self->mutex_);
          remove_delegate = self->stopped_;
          if (!remove_delegate) self->delegates_.push_back({tracks[i], tag});
        }
        if (remove_delegate) mk_track_del_delegate(tracks[i], tag);
      }
    }
  }

  static void OnShutdown(void* user, int, const char*, mk_track[], int) {
    auto* self = static_cast<ZlmInputBackend*>(user);
    std::lock_guard<std::mutex> lock(self->mutex_);
    self->reconnect_required_ = !self->stopped_;
    self->condition_.notify_all();
  }

  static void OnFrame(void* user, mk_frame frame) {
    auto* self = static_cast<ZlmInputBackend*>(user);
    if (self->stopped_ || !mk_frame_is_video(frame)) return;
    const char* data = mk_frame_get_data(frame);
    const std::size_t size = mk_frame_get_data_size(frame);
    std::vector<std::uint8_t> bytes(size);
    if (size != 0) std::copy(data, data + size, reinterpret_cast<char*>(bytes.data()));
    EncodedPacket packet;
    packet.pts =
        Timestamp::FromMicroseconds(static_cast<std::int64_t>(mk_frame_get_pts(frame)) * 1000);
    packet.dts =
        Timestamp::FromMicroseconds(static_cast<std::int64_t>(mk_frame_get_dts(frame)) * 1000);
    packet.codec = mk_frame_codec_name(frame);
    std::transform(packet.codec.begin(), packet.codec.end(), packet.codec.begin(),
                   [](unsigned char value) { return static_cast<char>(std::tolower(value)); });
    packet.key_frame = (mk_frame_get_flags(frame) & MK_FRAME_FLAG_IS_KEY) != 0;
    packet.buffer = std::make_shared<HostBuffer>(std::move(bytes));
    std::lock_guard<std::mutex> lock(self->mutex_);
    constexpr std::size_t kCapacity = 64;
    if (self->packets_.size() == kCapacity) self->packets_.pop_front();
    self->packets_.push_back(std::move(packet));
    self->condition_.notify_one();
  }

  StreamingSessionConfig config_;
  mk_player player_ = nullptr;
  std::vector<Delegate> delegates_;
  std::mutex player_mutex_;
  std::mutex mutex_;
  std::condition_variable condition_;
  std::deque<EncodedPacket> packets_;
  std::atomic<bool> stopped_{true};
  bool reconnect_required_ = false;
  int attempts_ = 0;
};

class ZlmOutputBackend final : public IStreamingOutputBackend {
 public:
  ~ZlmOutputBackend() override { Close(); }

  Status Open(const StreamingSessionConfig& config) override {
    Close();
    if (config.url.empty()) return Status::Invalid("streaming output URL is empty");
    EnsureEnvironment();
    config_ = config;
    return Status::Ok();
  }

  Status Publish(const EncodedPacket& packet) override {
    if (!packet.buffer) return Status::Invalid("streaming output packet has no buffer");
    const int codec_id = CodecId(packet.codec);
    if (codec_id < 0) return Status::Invalid("unsupported streaming codec: " + packet.codec);
    if (media_ == nullptr) {
      media_ = mk_media_create("__defaultVhost__", "rkavp", stream_id_.c_str(), 0, 0, 0);
      if (media_ == nullptr || !mk_media_init_video(media_, codec_id, 0, 0, 0, 0)) {
        Close();
        return Status::Unavailable("ZLMediaKit media source initialization failed");
      }
      mk_media_init_complete(media_);
      pusher_ = mk_pusher_create("rtsp", "__defaultVhost__", "rkavp", stream_id_.c_str());
      if (pusher_ == nullptr) {
        Close();
        return Status::Unavailable("mk_pusher_create failed");
      }
      mk_pusher_set_option(pusher_, "rtp_type", config_.transport == "udp" ? "0" : "1");
      mk_pusher_set_on_result(pusher_, &ZlmOutputBackend::OnPublish, this);
      mk_pusher_set_on_shutdown(pusher_, &ZlmOutputBackend::OnShutdown, this);
      mk_pusher_publish(pusher_, config_.url.c_str());
    }
    if (packet.buffer->fence()) {
      Status status = packet.buffer->fence()->Wait(-1);
      if (!status.ok()) return status;
    }
    Status status = packet.buffer->BeginCpuAccess(MapAccess::kRead);
    if (!status.ok()) return status;
    void* data = packet.buffer->Map(MapAccess::kRead);
    if (data == nullptr) {
      packet.buffer->EndCpuAccess(MapAccess::kRead);
      return Status::Invalid("streaming packet is not CPU mappable");
    }
    mk_frame frame = mk_frame_create(
        codec_id, packet.dts.is_range_value() ? packet.dts.microseconds() / 1000 : 0,
        packet.pts.is_range_value() ? packet.pts.microseconds() / 1000 : 0,
        static_cast<const char*>(data), packet.buffer->size(), nullptr, nullptr);
    int accepted = 0;
    if (frame != nullptr) {
      accepted = mk_media_input_frame(media_, frame);
      mk_frame_unref(frame);
    }
    packet.buffer->Unmap();
    status = packet.buffer->EndCpuAccess(MapAccess::kRead);
    if (!status.ok()) return status;
    if (frame == nullptr) return Status::Unavailable("mk_frame_create failed");
    return accepted ? Status::Ok() : Status::Unavailable("ZLMediaKit rejected encoded packet");
  }

  void SetKeyFrameRequestCallback(std::function<void()> callback) override {
    key_frame_callback_ = std::move(callback);
  }

  void Close() override {
    if (pusher_ != nullptr) {
      mk_pusher_set_on_result(pusher_, nullptr, nullptr);
      mk_pusher_set_on_shutdown(pusher_, nullptr, nullptr);
      mk_pusher_release(pusher_);
      pusher_ = nullptr;
    }
    if (media_ != nullptr) {
      mk_media_release(media_);
      media_ = nullptr;
    }
    connected_ = false;
  }

 private:
  static void OnPublish(void* user, int error, const char*) {
    auto* self = static_cast<ZlmOutputBackend*>(user);
    self->connected_ = error == 0;
    if (error == 0 && self->key_frame_callback_) self->key_frame_callback_();
  }
  static void OnShutdown(void* user, int, const char*) {
    auto* self = static_cast<ZlmOutputBackend*>(user);
    self->connected_ = false;
    if (self->key_frame_callback_) self->key_frame_callback_();
  }

  StreamingSessionConfig config_;
  const std::string stream_id_ = "output_" + std::to_string(reinterpret_cast<std::uintptr_t>(this));
  mk_media media_ = nullptr;
  mk_pusher pusher_ = nullptr;
  std::atomic<bool> connected_{false};
  std::function<void()> key_frame_callback_;
};

}  // namespace

std::unique_ptr<IStreamingInputBackend> CreateStreamingInputBackend() {
  return std::make_unique<ZlmInputBackend>();
}
std::unique_ptr<IStreamingOutputBackend> CreateStreamingOutputBackend() {
  return std::make_unique<ZlmOutputBackend>();
}

}  // namespace rkavp
