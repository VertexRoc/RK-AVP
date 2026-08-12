#include <atomic>
#include <chrono>
#include <thread>
#include <utility>

#include "rkavp/backends/rockchip_nodes.hpp"
#include "rkavp/packet_batch.hpp"

namespace rkavp {
namespace {

Status PositiveInteger(const NodeOptions& options, const std::string& key, int* output) {
  std::int64_t value = 0;
  Status status = GetIntegerOption(options, key, &value);
  if (!status.ok()) return status;
  if (value <= 0 || value > 0x7fffffff)
    return Status::Invalid("option must be a positive integer: " + key);
  *output = static_cast<int>(value);
  return Status::Ok();
}

Status OptionalPositiveInteger(const NodeOptions& options, const std::string& key, int* output) {
  if (FindOption(options, key) == nullptr) return Status::Ok();
  return PositiveInteger(options, key, output);
}

Status OptionalPositiveDuration(const NodeOptions& options, const std::string& key,
                                std::int64_t* output) {
  if (FindOption(options, key) == nullptr) return Status::Ok();
  std::int64_t value = 0;
  Status status = GetIntegerOption(options, key, &value);
  if (!status.ok()) return status;
  if (value <= 0) return Status::Invalid("option must be positive: " + key);
  *output = value;
  return Status::Ok();
}

class MppDecoderNode final : public Node {
 public:
  explicit MppDecoderNode(std::unique_ptr<IMppDecoderBackend> backend)
      : backend_(std::move(backend)) {}
  NodeContract Contract() const override {
    return {{{"packet", {MediaKind::kEncodedVideo, "", 0, 0, 0, 0, {}, {}}, true}},
            {{"frame",
              {MediaKind::kVideo, "", 0, 0, 0, 0, {MemoryType::kMpp, MemoryType::kDmaBuf}, {}},
              true}},
            {"codec"},
            InputPolicy::kAny,
            {}};
  }

 protected:
  Status OnConfigure(const NodeOptions& options) override {
    Status status = GetStringOption(options, "codec", &config_.codec);
    if (status.ok()) status = OptionalPositiveInteger(options, "width", &config_.width);
    if (status.ok()) status = OptionalPositiveInteger(options, "height", &config_.height);
    if (status.ok() && FindOption(options, "buffer_count") != nullptr) {
      int value = 0;
      status = PositiveInteger(options, "buffer_count", &value);
      config_.buffer_count = static_cast<std::size_t>(value);
    }
    if (status.ok() && FindOption(options, "external_buffer_group") != nullptr) {
      status = GetBoolOption(options, "external_buffer_group", &config_.external_buffer_group);
    }
    if (status.ok()) {
      status = OptionalPositiveDuration(options, "drain_timeout_ms", &config_.drain_timeout_ms);
    }
    return status;
  }
  Status OnOpen() override { return backend_->Open(config_); }
  Status OnProcess(NodeContext& context) override {
    const Packet* input = context.Input("packet");
    if (input == nullptr) return Status::Invalid("MppDecoder requires packet input");
    if (input->event() == ControlEvent::kEndOfStream) {
      Status status = backend_->Drain();
      if (!status.ok()) return status;
      status = EmitAvailable(context, true);
      if (!status.ok()) return status;
      return context.Emit("frame", Packet::Event(ControlEvent::kEndOfStream, Timestamp::Done()));
    }
    if (!input->Is<EncodedPacket>()) return Status::Invalid("MppDecoder expects EncodedPacket");
    Status status = backend_->Submit(input->Get<EncodedPacket>());
    return status.ok() ? EmitAvailable(context, false) : status;
  }
  Status OnClose() override {
    backend_->Close();
    return Status::Ok();
  }

 private:
  Status EmitAvailable(NodeContext& context, bool draining) {
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::milliseconds(config_.drain_timeout_ms);
    for (;;) {
      VideoFrame frame;
      bool end_of_stream = false;
      Status status = backend_->Receive(&frame, &end_of_stream);
      if (status.code() == StatusCode::kUnavailable) {
        if (!draining) return Status::Ok();
        if (std::chrono::steady_clock::now() >= deadline) {
          return Status::Unavailable("MPP decoder drain timed out");
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
        continue;
      }
      if (!status.ok()) return status;
      if (frame.buffer || !end_of_stream) {
        const Timestamp timestamp = frame.pts;
        status = context.Emit("frame", Packet::Make(std::move(frame), timestamp));
        if (!status.ok()) return status;
      }
      if (end_of_stream) return Status::Ok();
    }
  }
  std::unique_ptr<IMppDecoderBackend> backend_;
  MppDecoderConfig config_;
};

class MppEncoderNode final : public Node {
 public:
  explicit MppEncoderNode(std::unique_ptr<IMppEncoderBackend> backend)
      : backend_(std::move(backend)) {}
  NodeContract Contract() const override {
    return {{{"frame", {MediaKind::kVideo, "", 0, 0, 0, 0, {}, {}}, true}},
            {{"packet", {MediaKind::kEncodedVideo, "", 0, 0, 0, 0, {}, {}}, true}},
            {"codec", "width", "height", "fps", "bitrate"},
            InputPolicy::kAny,
            {}};
  }

 protected:
  Status OnConfigure(const NodeOptions& options) override {
    Status status = GetStringOption(options, "codec", &config_.codec);
    if (status.ok()) status = PositiveInteger(options, "width", &config_.width);
    if (status.ok()) status = PositiveInteger(options, "height", &config_.height);
    if (status.ok()) status = PositiveInteger(options, "fps", &config_.fps);
    if (status.ok()) status = PositiveInteger(options, "bitrate", &config_.bitrate);
    if (FindOption(options, "gop") != nullptr) {
      status = PositiveInteger(options, "gop", &config_.gop);
    }
    if (status.ok() && FindOption(options, "profile") != nullptr) {
      status = GetStringOption(options, "profile", &config_.profile);
    }
    if (status.ok() && FindOption(options, "rate_control") != nullptr) {
      status = GetStringOption(options, "rate_control", &config_.rate_control);
    }
    if (status.ok()) {
      status = OptionalPositiveDuration(options, "drain_timeout_ms", &config_.drain_timeout_ms);
    }
    return status;
  }
  Status OnOpen() override { return backend_->Open(config_); }
  Status OnProcess(NodeContext& context) override {
    const Packet* input = context.Input("frame");
    if (input == nullptr) return Status::Invalid("MppEncoder requires frame input");
    if (input->event() == ControlEvent::kForceKeyFrame) return backend_->ForceKeyFrame();
    if (input->event() == ControlEvent::kEndOfStream) {
      Status status = backend_->Drain();
      if (!status.ok()) return status;
      status = EmitAvailable(context, true);
      if (!status.ok()) return status;
      return context.Emit("packet", Packet::Event(ControlEvent::kEndOfStream, Timestamp::Done()));
    }
    if (!input->Is<VideoFrame>()) return Status::Invalid("MppEncoder expects VideoFrame");
    Status status = backend_->Submit(input->Get<VideoFrame>());
    return status.ok() ? EmitAvailable(context, false) : status;
  }
  Status OnClose() override {
    backend_->Close();
    return Status::Ok();
  }

 private:
  Status EmitAvailable(NodeContext& context, bool draining) {
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::milliseconds(config_.drain_timeout_ms);
    for (;;) {
      EncodedPacket packet;
      bool end_of_stream = false;
      Status status = backend_->Receive(&packet, &end_of_stream);
      if (status.code() == StatusCode::kUnavailable) {
        if (!draining) return Status::Ok();
        if (std::chrono::steady_clock::now() >= deadline) {
          return Status::Unavailable("MPP encoder drain timed out");
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
        continue;
      }
      if (!status.ok()) return status;
      if (packet.buffer || !end_of_stream) {
        const Timestamp timestamp = packet.pts;
        status = context.Emit("packet", Packet::Make(std::move(packet), timestamp));
        if (!status.ok()) return status;
      }
      if (end_of_stream) return Status::Ok();
    }
  }
  std::unique_ptr<IMppEncoderBackend> backend_;
  MppEncoderConfig config_;
};

class RgaTransformNode final : public Node {
 public:
  explicit RgaTransformNode(std::unique_ptr<IRgaBackend> backend) : backend_(std::move(backend)) {}
  NodeContract Contract() const override {
    return {{{"frame", {MediaKind::kVideo, "", 0, 0, 0, 0, {}, {}}, true}},
            {{"frame", {MediaKind::kVideo, "", 0, 0, 0, 0, {}, {}}, true}},
            {"width", "height", "format"},
            InputPolicy::kAny,
            {}};
  }

 protected:
  Status OnConfigure(const NodeOptions& options) override {
    Status status = PositiveInteger(options, "width", &request_.destination_width);
    if (status.ok()) status = PositiveInteger(options, "height", &request_.destination_height);
    if (status.ok()) status = GetStringOption(options, "format", &request_.destination_format);
    if (status.ok() && FindOption(options, "rotation") != nullptr) {
      std::int64_t rotation = 0;
      status = GetIntegerOption(options, "rotation", &rotation);
      if (status.ok() && rotation != 0 && rotation != 90 && rotation != 180 && rotation != 270) {
        status = Status::Invalid("RGA rotation must be 0, 90, 180, or 270 degrees");
      }
      request_.rotation_degrees = static_cast<int>(rotation);
    }
    if (status.ok() && FindOption(options, "asynchronous") != nullptr) {
      status = GetBoolOption(options, "asynchronous", &request_.asynchronous);
    }
    return status;
  }
  Status OnProcess(NodeContext& context) override {
    const Packet* input = context.Input("frame");
    if (input == nullptr || !input->Is<VideoFrame>())
      return Status::Invalid("RgaTransform expects VideoFrame");
    VideoFrame output;
    Status status = backend_->Transform(input->Get<VideoFrame>(), request_, &output);
    if (!status.ok()) return status;
    output.frame_id = input->Get<VideoFrame>().frame_id;
    output.pts = input->Get<VideoFrame>().pts;
    return context.Emit("frame", Packet::Make(std::move(output), input->timestamp()));
  }
  Status OnClose() override {
    backend_->ReleaseCachedHandles();
    return Status::Ok();
  }

 private:
  std::unique_ptr<IRgaBackend> backend_;
  RgaTransformRequest request_;
};

class RknnInferenceNode final : public Node {
 public:
  explicit RknnInferenceNode(std::unique_ptr<IRknnBackend> backend)
      : backend_(std::move(backend)) {}
  NodeContract Contract() const override {
    return {{{"tensors", {MediaKind::kTensor, "", 0, 0, 0, 0, {}, {}}, true}},
            {{"tensors", {MediaKind::kTensor, "", 0, 0, 0, 0, {}, {}}, true}},
            {"model"},
            InputPolicy::kAny,
            {}};
  }

 protected:
  Status OnConfigure(const NodeOptions& options) override {
    Status status = GetStringOption(options, "model", &config_.path);
    if (status.ok() && FindOption(options, "zero_copy") != nullptr) {
      status = GetBoolOption(options, "zero_copy", &config_.zero_copy);
    }
    if (status.ok() && FindOption(options, "cache_tensor_memory") != nullptr) {
      status = GetBoolOption(options, "cache_tensor_memory", &config_.cache_tensor_memory);
    }
    if (status.ok() && FindOption(options, "core_mask") != nullptr) {
      std::string value;
      status = GetStringOption(options, "core_mask", &value);
      if (value == "auto")
        config_.core_mask = RknnCoreMask::kAuto;
      else if (value == "0")
        config_.core_mask = RknnCoreMask::kCore0;
      else if (value == "1")
        config_.core_mask = RknnCoreMask::kCore1;
      else if (value == "2")
        config_.core_mask = RknnCoreMask::kCore2;
      else if (value == "0_1")
        config_.core_mask = RknnCoreMask::kCore01;
      else if (value == "0_1_2")
        config_.core_mask = RknnCoreMask::kCore012;
      else if (status.ok())
        status = Status::Invalid("RKNN core_mask must be auto, 0, 1, 2, 0_1, or 0_1_2");
    }
    return status;
  }
  Status OnOpen() override {
    Status status = backend_->LoadModel(config_);
    if (status.ok()) status = backend_->QueryInputs(&inputs_);
    if (status.ok()) status = backend_->QueryOutputs(&outputs_);
    return status;
  }
  Status OnProcess(NodeContext& context) override {
    const Packet* input = context.Input("tensors");
    if (input == nullptr || !input->Is<TensorSet>())
      return Status::Invalid("RknnInference expects TensorSet");
    TensorSet outputs;
    Status status = backend_->Infer(input->Get<TensorSet>(), &outputs);
    if (!status.ok()) return status;
    return context.Emit("tensors", Packet::Make(std::move(outputs), input->timestamp()));
  }
  Status OnClose() override {
    backend_->Close();
    return Status::Ok();
  }

 private:
  std::unique_ptr<IRknnBackend> backend_;
  RknnModelConfig config_;
  std::vector<TensorDesc> inputs_;
  std::vector<TensorDesc> outputs_;
};

class RknnBatchInferenceNode final : public Node {
 public:
  explicit RknnBatchInferenceNode(std::unique_ptr<IRknnBackend> backend)
      : backend_(std::move(backend)) {}
  NodeContract Contract() const override {
    return {{{"batch", MediaCaps::Any(), true}},
            {{"batch", MediaCaps::Any(), true}},
            {"model"},
            InputPolicy::kAny,
            {}};
  }

 protected:
  Status OnConfigure(const NodeOptions& options) override {
    return GetStringOption(options, "model", &config_.path);
  }
  Status OnOpen() override { return backend_->LoadModel(config_); }
  Status OnProcess(NodeContext& context) override {
    const Packet* input = context.Input("batch");
    if (input == nullptr || !input->Is<PacketBatch>()) {
      return Status::Invalid("RknnBatchInference expects PacketBatch");
    }
    std::vector<BatchItem> outputs;
    outputs.reserve(input->Get<PacketBatch>().size());
    for (const auto& item : input->Get<PacketBatch>().items()) {
      if (!item.packet.Is<TensorSet>()) return Status::Invalid("RKNN batch item expects TensorSet");
      TensorSet tensors;
      Status status = backend_->Infer(item.packet.Get<TensorSet>(), &tensors);
      if (!status.ok()) return status;
      Packet packet = Packet::Make(std::move(tensors), item.packet.timestamp());
      packet.mutable_metadata() = item.packet.metadata();
      outputs.push_back({item.source_id, std::move(packet), item.batch_index});
    }
    context.metrics()->Increment("rknn.batch.fallback_runs", outputs.size());
    return context.Emit("batch", MakePacketBatch(std::move(outputs), input->timestamp()));
  }
  Status OnClose() override {
    backend_->Close();
    return Status::Ok();
  }

 private:
  std::unique_ptr<IRknnBackend> backend_;
  RknnModelConfig config_;
};

class V4l2SourceNode final : public Node {
 public:
  explicit V4l2SourceNode(std::unique_ptr<IV4l2Backend> backend) : backend_(std::move(backend)) {}
  ~V4l2SourceNode() override { StopThread(); }
  NodeContract Contract() const override {
    return {{},
            {{"packet", MediaCaps::Any(), true}},
            {"device", "width", "height", "fps", "format"},
            InputPolicy::kAny,
            {}};
  }

 protected:
  Status OnConfigure(const NodeOptions& options) override {
    Status status = GetStringOption(options, "device", &config_.device);
    if (status.ok()) status = PositiveInteger(options, "width", &config_.width);
    if (status.ok()) status = PositiveInteger(options, "height", &config_.height);
    if (status.ok()) status = PositiveInteger(options, "fps", &config_.fps);
    if (status.ok()) status = GetStringOption(options, "format", &config_.format);
    if (FindOption(options, "io_mode") != nullptr) {
      std::string value;
      status = GetStringOption(options, "io_mode", &value);
      if (status.ok() && value == "mmap")
        config_.io_mode = V4l2IoMode::kMmap;
      else if (status.ok() && value == "export_dmabuf")
        config_.io_mode = V4l2IoMode::kExportDmaBuf;
      else if (status.ok() && value == "dmabuf") {
        status = Status::Invalid(
            "V4L2 io_mode=dmabuf is ambiguous; use export_dmabuf for MMAP buffers exported as "
            "DMA-BUF");
      } else if (status.ok())
        status = Status::Invalid("V4L2 io_mode must be mmap or export_dmabuf");
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
      Packet packet;
      Status status = backend_->Read(&packet);
      if (status.code() == StatusCode::kUnavailable) continue;
      if (!status.ok()) {
        if (!stop_) context_->metrics()->Increment("v4l2.errors");
        break;
      }
      status = context_->Emit("packet", std::move(packet));
      if (!status.ok()) break;
    }
  }
  void StopThread() {
    stop_ = true;
    backend_->RequestStop();
    if (thread_.joinable()) thread_.join();
  }
  std::unique_ptr<IV4l2Backend> backend_;
  V4l2CaptureConfig config_;
  NodeContext* context_ = nullptr;
  std::atomic<bool> stop_{false};
  std::thread thread_;
};

}  // namespace

std::unique_ptr<Node> MakeMppDecoderNode(std::unique_ptr<IMppDecoderBackend> backend) {
  return std::make_unique<MppDecoderNode>(std::move(backend));
}
std::unique_ptr<Node> MakeMppEncoderNode(std::unique_ptr<IMppEncoderBackend> backend) {
  return std::make_unique<MppEncoderNode>(std::move(backend));
}
std::unique_ptr<Node> MakeRgaTransformNode(std::unique_ptr<IRgaBackend> backend) {
  return std::make_unique<RgaTransformNode>(std::move(backend));
}
std::unique_ptr<Node> MakeRknnInferenceNode(std::unique_ptr<IRknnBackend> backend) {
  return std::make_unique<RknnInferenceNode>(std::move(backend));
}
std::unique_ptr<Node> MakeRknnBatchInferenceNode(std::unique_ptr<IRknnBackend> backend) {
  return std::make_unique<RknnBatchInferenceNode>(std::move(backend));
}
std::unique_ptr<Node> MakeV4l2SourceNode(std::unique_ptr<IV4l2Backend> backend) {
  return std::make_unique<V4l2SourceNode>(std::move(backend));
}

Status RegisterRockchipNodes(NodeRegistry* registry,
                             std::shared_ptr<RockchipBackendFactory> factory) {
  if (registry == nullptr || !factory)
    return Status::Invalid("registry and Rockchip backend factory are required");
  Status status = registry->Register(
      "MppDecoder", [factory] { return MakeMppDecoderNode(factory->CreateMppDecoder()); });
  if (status.ok())
    status = registry->Register(
        "MppEncoder", [factory] { return MakeMppEncoderNode(factory->CreateMppEncoder()); });
  if (status.ok())
    status = registry->Register("RgaTransform",
                                [factory] { return MakeRgaTransformNode(factory->CreateRga()); });
  if (status.ok())
    status = registry->Register("RknnInference",
                                [factory] { return MakeRknnInferenceNode(factory->CreateRknn()); });
  if (status.ok())
    status = registry->Register("RknnBatchInference", [factory] {
      return MakeRknnBatchInferenceNode(factory->CreateRknn());
    });
  if (status.ok())
    status = registry->Register("V4l2Source",
                                [factory] { return MakeV4l2SourceNode(factory->CreateV4l2()); });
  return status;
}

}  // namespace rkavp
