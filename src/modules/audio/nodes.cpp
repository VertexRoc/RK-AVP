#include <atomic>
#include <thread>
#include <utility>

#include "rkavp/backends/audio_nodes.hpp"

namespace rkavp {
namespace {

class AlsaCaptureNode final : public Node {
 public:
  explicit AlsaCaptureNode(std::unique_ptr<IAlsaBackend> backend) : backend_(std::move(backend)) {}
  ~AlsaCaptureNode() override { StopThread(); }
  NodeContract Contract() const override {
    return {{},
            {{"audio", {MediaKind::kAudio}, true}},
            {"device", "sample_rate", "channels", "format", "frame_ms"}};
  }

 protected:
  Status OnConfigure(const NodeOptions& options) override {
    Status status = GetStringOption(options, "device", &config_.device);
    std::int64_t value = 0;
    if (status.ok()) status = GetIntegerOption(options, "sample_rate", &value);
    config_.sample_rate = static_cast<int>(value);
    if (status.ok()) status = GetIntegerOption(options, "channels", &value);
    config_.channels = static_cast<int>(value);
    if (status.ok()) status = GetStringOption(options, "format", &config_.format);
    if (status.ok()) status = GetIntegerOption(options, "frame_ms", &value);
    config_.frame_ms = static_cast<int>(value);
    if (config_.sample_rate <= 0 || config_.channels <= 0 || config_.frame_ms <= 0) {
      return Status::Invalid("ALSA sample_rate, channels and frame_ms must be positive");
    }
    return status;
  }
  Status OnOpen() override { return backend_->Open(config_); }
  Status OnStart(NodeContext& context) override {
    context_ = &context;
    stop_ = false;
    thread_ = std::thread([this] { Loop(); });
    return Status::Ok();
  }
  Status OnProcess(NodeContext&) override { return Status::Ok(); }
  void OnRequestStop() override {
    stop_ = true;
    backend_->RequestStop();
  }
  Status OnStop() override {
    StopThread();
    return Status::Ok();
  }
  Status OnClose() override {
    backend_->Close();
    return Status::Ok();
  }

 private:
  void Loop() {
    while (!stop_ && !context_->cancelled()) {
      AudioFrame frame;
      Status status = backend_->Read(&frame);
      if (status.code() == StatusCode::kUnavailable) continue;
      if (status.code() == StatusCode::kCancelled) break;
      if (!status.ok()) {
        if (status.code() != StatusCode::kInternal) {
          context_->metrics()->Increment("alsa.capture_errors");
          break;
        }
        context_->metrics()->Increment("alsa.overruns");
        status = backend_->RecoverOverrun();
        if (!status.ok()) break;
        continue;
      }
      const Timestamp timestamp = frame.pts;
      status = context_->Emit("audio", Packet::Make(std::move(frame), timestamp));
      if (!status.ok()) break;
    }
  }
  void StopThread() {
    stop_ = true;
    backend_->RequestStop();
    if (thread_.joinable()) thread_.join();
  }
  std::unique_ptr<IAlsaBackend> backend_;
  AlsaCaptureConfig config_;
  NodeContext* context_ = nullptr;
  std::atomic<bool> stop_{false};
  std::thread thread_;
};

}  // namespace

std::unique_ptr<Node> MakeAlsaCaptureNode(std::unique_ptr<IAlsaBackend> backend) {
  return std::make_unique<AlsaCaptureNode>(std::move(backend));
}

Status RegisterAudioNodes(NodeRegistry* registry,
                          std::function<std::unique_ptr<IAlsaBackend>()> factory) {
  if (registry == nullptr || !factory)
    return Status::Invalid("registry and ALSA backend factory are required");
  return registry->Register("AlsaCapture", [factory] { return MakeAlsaCaptureNode(factory()); });
}

}  // namespace rkavp
