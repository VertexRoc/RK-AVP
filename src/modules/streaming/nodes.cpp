#include <atomic>
#include <thread>
#include <utility>

#include "rkavp/backends/streaming_nodes.hpp"

namespace rkavp {
namespace {

Status ConfigureSession(const NodeOptions& options, StreamingSessionConfig* config) {
  Status status = GetStringOption(options, "url", &config->url);
  if (const ConfigValue* transport = FindOption(options, "transport")) {
    (void)transport;
    status = GetStringOption(options, "transport", &config->transport);
  }
  return status;
}

class StreamingInputNode final : public Node {
 public:
  explicit StreamingInputNode(std::unique_ptr<IStreamingInputBackend> backend)
      : backend_(std::move(backend)) {}
  ~StreamingInputNode() override { StopThread(); }
  NodeContract Contract() const override {
    return {{}, {{"packet", {MediaKind::kEncodedVideo}, true}}, {"url"}};
  }

 protected:
  Status OnConfigure(const NodeOptions& options) override {
    return ConfigureSession(options, &config_);
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
      EncodedPacket packet;
      Status status = backend_->Read(&packet);
      if (status.code() == StatusCode::kUnavailable) continue;
      if (status.code() == StatusCode::kCancelled) break;
      if (!status.ok()) {
        context_->metrics()->Increment("streaming.input_errors");
        break;
      }
      const Timestamp timestamp = packet.pts;
      if (!context_->Emit("packet", Packet::Make(std::move(packet), timestamp)).ok()) break;
    }
  }
  void StopThread() {
    stop_ = true;
    backend_->RequestStop();
    if (thread_.joinable()) thread_.join();
  }
  std::unique_ptr<IStreamingInputBackend> backend_;
  StreamingSessionConfig config_;
  NodeContext* context_ = nullptr;
  std::atomic<bool> stop_{false};
  std::thread thread_;
};

class StreamingOutputNode final : public Node {
 public:
  explicit StreamingOutputNode(std::unique_ptr<IStreamingOutputBackend> backend)
      : backend_(std::move(backend)) {}
  NodeContract Contract() const override {
    return {{{"packet", {MediaKind::kEncodedVideo}, true}}, {}, {"url"}};
  }

 protected:
  Status OnConfigure(const NodeOptions& options) override {
    return ConfigureSession(options, &config_);
  }
  Status OnOpen() override { return backend_->Open(config_); }
  Status OnStart(NodeContext& context) override {
    context_ = &context;
    backend_->SetKeyFrameRequestCallback([this] {
      if (context_ != nullptr) context_->metrics()->Increment("streaming.keyframe_requests");
    });
    return Status::Ok();
  }
  Status OnProcess(NodeContext& context) override {
    const Packet* packet = context.Input("packet");
    if (packet == nullptr) return Status::Invalid("StreamingOutput requires packet input");
    if (packet->event() == ControlEvent::kEndOfStream) return Status::Ok();
    if (!packet->Is<EncodedPacket>())
      return Status::Invalid("StreamingOutput expects EncodedPacket");
    return backend_->Publish(packet->Get<EncodedPacket>());
  }
  Status OnClose() override {
    backend_->Close();
    return Status::Ok();
  }

 private:
  std::unique_ptr<IStreamingOutputBackend> backend_;
  StreamingSessionConfig config_;
  NodeContext* context_ = nullptr;
};

}  // namespace

std::unique_ptr<Node> MakeStreamingInputNode(std::unique_ptr<IStreamingInputBackend> backend) {
  return std::make_unique<StreamingInputNode>(std::move(backend));
}
std::unique_ptr<Node> MakeStreamingOutputNode(std::unique_ptr<IStreamingOutputBackend> backend) {
  return std::make_unique<StreamingOutputNode>(std::move(backend));
}
Status RegisterStreamingNodes(
    NodeRegistry* registry, std::function<std::unique_ptr<IStreamingInputBackend>()> input_factory,
    std::function<std::unique_ptr<IStreamingOutputBackend>()> output_factory) {
  if (registry == nullptr || !input_factory || !output_factory)
    return Status::Invalid("streaming factories are required");
  Status status = registry->Register(
      "StreamingInput", [input_factory] { return MakeStreamingInputNode(input_factory()); });
  if (status.ok())
    status = registry->Register(
        "StreamingOutput", [output_factory] { return MakeStreamingOutputNode(output_factory()); });
  return status;
}

}  // namespace rkavp
