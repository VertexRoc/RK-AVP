#include <gtest/gtest.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <future>
#include <memory>
#include <mutex>

#include "rkavp/audio.hpp"
#include "rkavp/opencl.hpp"
#include "rkavp/rockchip.hpp"
#include "rkavp/streaming.hpp"

namespace rkavp {
namespace {

class RecordingMppDecoder final : public IMppDecoderBackend {
 public:
  Status Open(const MppDecoderConfig& value) override {
    config = value;
    return Status::Ok();
  }
  Status Submit(const EncodedPacket& packet) override {
    VideoFrame frame;
    frame.frame_id = 17;
    frame.pts = packet.pts;
    frames.push_back(std::move(frame));
    return Status::Ok();
  }
  Status Receive(VideoFrame* frame, bool* end_of_stream) override {
    *end_of_stream = false;
    if (frames.empty()) {
      if (!drained || eos_reported) return Status::Unavailable("no frame");
      eos_reported = true;
      *end_of_stream = true;
      return Status::Ok();
    }
    *frame = std::move(frames.front());
    frames.pop_front();
    return Status::Ok();
  }
  Status Drain() override {
    drained = true;
    return Status::Ok();
  }
  void Close() override { closed = true; }
  MppDecoderConfig config;
  std::deque<VideoFrame> frames;
  bool drained = false;
  bool eos_reported = false;
  bool closed = false;
};

class RecordingMppEncoder final : public IMppEncoderBackend {
 public:
  Status Open(const MppEncoderConfig& value) override {
    config = value;
    return Status::Ok();
  }
  Status Submit(const VideoFrame& frame) override {
    EncodedPacket packet;
    packet.pts = frame.pts;
    packets.push_back(std::move(packet));
    return Status::Ok();
  }
  Status Receive(EncodedPacket* packet, bool* end_of_stream) override {
    *end_of_stream = false;
    if (packets.empty()) {
      if (!drained || eos_reported) return Status::Unavailable("no packet");
      eos_reported = true;
      *end_of_stream = true;
      return Status::Ok();
    }
    *packet = std::move(packets.front());
    packets.pop_front();
    return Status::Ok();
  }
  Status Drain() override {
    drained = true;
    return Status::Ok();
  }
  Status ForceKeyFrame() override {
    keyframe = true;
    return Status::Ok();
  }
  void Close() override {}
  MppEncoderConfig config;
  std::deque<EncodedPacket> packets;
  bool drained = false;
  bool eos_reported = false;
  bool keyframe = false;
};

class RecordingRga final : public IRgaBackend {
 public:
  Status Transform(const VideoFrame& source, const RgaTransformRequest& value,
                   VideoFrame* output) override {
    request = value;
    source_fd = source.buffer->planes().front().fd;
    output->width = value.destination_width;
    output->height = value.destination_height;
    output->format = value.destination_format;
    return Status::Ok();
  }
  Status Blit(const VideoFrame& source, const RgaTransformRequest& value,
              VideoFrame* output) override {
    return Transform(source, value, output);
  }
  Status Composite(const VideoFrame&, const VideoFrame&, const RgaCompositeRequest&,
                   VideoFrame*) override {
    return Status::Ok();
  }
  void ReleaseCachedHandles() override { released = true; }
  RgaTransformRequest request;
  int source_fd = -1;
  bool released = false;
};

class RecordingRknn final : public IRknnBackend {
 public:
  Status LoadModel(const RknnModelConfig& value) override {
    config = value;
    ++load_count;
    return Status::Ok();
  }
  Status QueryInputs(std::vector<TensorDesc>* value) const override {
    *value = input_descs;
    return Status::Ok();
  }
  Status QueryOutputs(std::vector<TensorDesc>* value) const override {
    *value = output_descs;
    return Status::Ok();
  }
  Status SetDynamicShape(const std::vector<std::vector<std::int64_t>>& value) override {
    shapes = value;
    return Status::Ok();
  }
  Status Infer(const TensorSet& inputs, TensorSet* outputs) override {
    ++infer_count;
    if (fail) return Status::Internal("inference failed");
    *outputs = inputs;
    return Status::Ok();
  }
  Status QueryPerformance(RknnPerformance* value) const override {
    value->run_duration_us = 10;
    return Status::Ok();
  }
  void Close() override { closed = true; }
  RknnModelConfig config;
  std::vector<TensorDesc> input_descs{
      {"image", {1, 224, 224, 3}, TensorDataType::kUInt8, TensorLayout::kNhwc}};
  std::vector<TensorDesc> output_descs{
      {"embedding", {1, 512}, TensorDataType::kFloat32, TensorLayout::kAny}};
  std::vector<std::vector<std::int64_t>> shapes;
  int load_count = 0;
  int infer_count = 0;
  bool fail = false;
  bool closed = false;
};

class RecoveringAlsa final : public IAlsaBackend {
 public:
  Status Open(const AlsaCaptureConfig&) override { return Status::Ok(); }
  Status Read(AudioFrame*) override {
    if (!reported_overrun.exchange(true)) {
      return Status::Internal("overrun");
    }
    stop_future.wait();
    return Status::Cancelled("stopped");
  }
  Status RecoverOverrun() override {
    if (!recovered.exchange(true)) recovered_promise.set_value();
    return Status::Ok();
  }
  void RequestStop() override {
    if (!stopped.exchange(true)) stop_promise.set_value();
  }
  void Close() override {}
  bool WaitForRecovery() {
    return recovered_future.wait_for(std::chrono::seconds(1)) == std::future_status::ready;
  }
  std::atomic<bool> reported_overrun{false};
  std::atomic<bool> recovered{false};
  std::atomic<bool> stopped{false};
  std::promise<void> recovered_promise;
  std::shared_future<void> recovered_future = recovered_promise.get_future().share();
  std::promise<void> stop_promise;
  std::shared_future<void> stop_future = stop_promise.get_future().share();
};

class BlockingStreamingInput final : public IStreamingInputBackend {
 public:
  Status Open(const StreamingSessionConfig&) override { return Status::Ok(); }
  Status Read(EncodedPacket*) override {
    reading.store(true);
    if (!reading_reported.exchange(true)) reading_promise.set_value();
    stop_future.wait();
    return Status::Cancelled("stopped");
  }
  void RequestStop() override {
    if (!stopped.exchange(true)) stop_promise.set_value();
  }
  void Close() override {}
  bool WaitUntilReading() {
    return reading_future.wait_for(std::chrono::seconds(1)) == std::future_status::ready;
  }
  std::atomic<bool> reading{false};
  std::atomic<bool> reading_reported{false};
  std::atomic<bool> stopped{false};
  std::promise<void> reading_promise;
  std::shared_future<void> reading_future = reading_promise.get_future().share();
  std::promise<void> stop_promise;
  std::shared_future<void> stop_future = stop_promise.get_future().share();
};

class ReadyFence final : public Fence {
 public:
  Status Wait(std::int64_t) override {
    ++wait_count;
    return Status::Ok();
  }
  int wait_count = 0;
};

class RecordingOpenCl final : public IOpenClBackend {
 public:
  Status Compile(const OpenClKernelConfig& value) override {
    compiled = value;
    return Status::Ok();
  }
  Status Run(const std::string& kernel, const std::vector<BufferPtr>& inputs,
             const std::vector<BufferPtr>& outputs, const std::vector<std::size_t>& work_size,
             std::shared_ptr<Fence>* output_fence) override {
    invoked_kernel = kernel;
    input_count = inputs.size();
    output_count = outputs.size();
    global_work_size = work_size;
    fence = std::make_shared<ReadyFence>();
    *output_fence = fence;
    return Status::Ok();
  }
  void Close() override { closed = true; }
  OpenClKernelConfig compiled;
  std::string invoked_kernel;
  std::size_t input_count = 0;
  std::size_t output_count = 0;
  std::vector<std::size_t> global_work_size;
  std::shared_ptr<ReadyFence> fence;
  bool closed = false;
};

NodeContext MakeContext(PacketSet inputs, Packet* output) {
  static std::atomic<bool> cancelled{false};
  static MetricsRegistry metrics;
  return NodeContext(
      "node", "default", std::move(inputs),
      [output](const std::string&, Packet packet) {
        *output = std::move(packet);
        return Status::Ok();
      },
      nullptr, nullptr, nullptr, &metrics, &cancelled);
}

TEST(HardwareBackendTest, PassesDmaBufGeometryToRga) {
  int descriptors[2];
  ASSERT_EQ(::pipe(descriptors), 0);
  RecordingRga rga;
  VideoFrame source;
  source.buffer = DmaBuffer::Adopt(descriptors[0], 4096, {{descriptors[0], 0, 4096, 1920, 1, 0}});
  VideoFrame destination;
  RgaTransformRequest request;
  request.destination_width = 640;
  request.destination_height = 640;
  request.destination_format = "rgb888";
  ASSERT_TRUE(rga.Transform(source, request, &destination).ok());
  EXPECT_EQ(rga.source_fd, descriptors[0]);
  EXPECT_EQ(destination.width, 640);
  ::close(descriptors[1]);
}

TEST(HardwareBackendTest, SupportsGenericMultiInputRknnTensors) {
  RecordingRknn rknn;
  ASSERT_TRUE(rknn.LoadModel({"model.rknn", RknnCoreMask::kCore01, true, true}).ok());
  Tensor image{{"image", {1, 3, 32, 32}, TensorDataType::kUInt8, TensorLayout::kNchw},
               std::make_shared<HostBuffer>(3072)};
  Tensor text{{"tokens", {1, 77}, TensorDataType::kInt32, TensorLayout::kAny},
              std::make_shared<HostBuffer>(308)};
  TensorSet outputs;
  ASSERT_TRUE(rknn.Infer({image, text}, &outputs).ok());
  EXPECT_EQ(outputs.size(), 2U);
  EXPECT_EQ(rknn.load_count, 1);
}

TEST(HardwareNodeTest, DecoderPreservesTimestampAndDrainsEos) {
  auto backend = std::make_unique<RecordingMppDecoder>();
  RecordingMppDecoder* raw = backend.get();
  auto node = MakeMppDecoderNode(std::move(backend));
  ASSERT_TRUE(node->Configure({{"codec", "h264"}}).ok());
  ASSERT_TRUE(node->Open().ok());
  Packet output;
  NodeContext start = MakeContext({}, &output);
  ASSERT_TRUE(node->Start(start).ok());
  EncodedPacket encoded;
  encoded.pts = Timestamp::FromMicroseconds(123);
  NodeContext process = MakeContext({{"packet", Packet::Make(encoded, encoded.pts)}}, &output);
  ASSERT_TRUE(node->Process(process).ok());
  ASSERT_TRUE(output.Is<VideoFrame>());
  EXPECT_EQ(output.timestamp(), encoded.pts);
  NodeContext eos = MakeContext(
      {{"packet", Packet::Event(ControlEvent::kEndOfStream, Timestamp::Done())}}, &output);
  ASSERT_TRUE(node->Process(eos).ok());
  EXPECT_TRUE(raw->drained);
}

TEST(HardwareNodeTest, EncoderDrainsPacketsBeforePropagatingEos) {
  auto backend = std::make_unique<RecordingMppEncoder>();
  RecordingMppEncoder* raw = backend.get();
  auto node = MakeMppEncoderNode(std::move(backend));
  ASSERT_TRUE(node->Configure({{"codec", "h264"},
                               {"width", 1920},
                               {"height", 1080},
                               {"fps", 30},
                               {"bitrate", 4000000}})
                  .ok());
  ASSERT_TRUE(node->Open().ok());

  std::vector<Packet> outputs;
  std::atomic<bool> cancelled{false};
  MetricsRegistry metrics;
  auto make_context = [&](PacketSet inputs) {
    return NodeContext(
        "encoder", "video", std::move(inputs),
        [&outputs](const std::string&, Packet packet) {
          outputs.push_back(std::move(packet));
          return Status::Ok();
        },
        nullptr, nullptr, nullptr, &metrics, &cancelled);
  };

  NodeContext start = make_context({});
  ASSERT_TRUE(node->Start(start).ok());
  VideoFrame frame;
  frame.pts = Timestamp::FromMicroseconds(456);
  NodeContext process = make_context({{"frame", Packet::Make(frame, frame.pts)}});
  ASSERT_TRUE(node->Process(process).ok());
  ASSERT_EQ(outputs.size(), 1U);
  EXPECT_TRUE(outputs.front().Is<EncodedPacket>());
  EXPECT_EQ(outputs.front().timestamp(), frame.pts);

  NodeContext eos =
      make_context({{"frame", Packet::Event(ControlEvent::kEndOfStream, Timestamp::Done())}});
  ASSERT_TRUE(node->Process(eos).ok());
  EXPECT_TRUE(raw->drained);
  ASSERT_EQ(outputs.size(), 2U);
  EXPECT_EQ(outputs.back().event(), ControlEvent::kEndOfStream);
}

TEST(HardwareNodeTest, RknnNodeProducesTensorSetAndPropagatesFailure) {
  auto backend = std::make_unique<RecordingRknn>();
  RecordingRknn* raw = backend.get();
  auto node = MakeRknnInferenceNode(std::move(backend));
  ASSERT_TRUE(node->Configure({{"model", "generic.rknn"}}).ok());
  ASSERT_TRUE(node->Open().ok());
  Packet output;
  NodeContext start = MakeContext({}, &output);
  ASSERT_TRUE(node->Start(start).ok());
  Tensor input{{"input", {1}, TensorDataType::kFloat32, TensorLayout::kAny},
               std::make_shared<HostBuffer>(4)};
  NodeContext process = MakeContext(
      {{"tensors", Packet::Make(TensorSet{input}, Timestamp::FromMicroseconds(9))}}, &output);
  ASSERT_TRUE(node->Process(process).ok());
  ASSERT_TRUE(output.Is<TensorSet>());
  raw->fail = true;
  EXPECT_EQ(node->Process(process).code(), StatusCode::kInternal);
}

TEST(HardwareNodeTest, RknnBatchFallbackPreservesSourceAndFrameMapping) {
  auto backend = std::make_unique<RecordingRknn>();
  RecordingRknn* raw = backend.get();
  auto node = MakeRknnBatchInferenceNode(std::move(backend));
  ASSERT_TRUE(node->Configure({{"model", "generic.rknn"}}).ok());
  ASSERT_TRUE(node->Open().ok());
  Packet output;
  NodeContext start = MakeContext({}, &output);
  ASSERT_TRUE(node->Start(start).ok());
  std::vector<BatchItem> items;
  for (int index = 0; index < 2; ++index) {
    Tensor tensor{{"input", {1}, TensorDataType::kFloat32, TensorLayout::kAny},
                  std::make_shared<HostBuffer>(4)};
    Packet packet = Packet::Make(TensorSet{tensor}, Timestamp::FromMicroseconds(index + 1));
    packet.mutable_metadata().Set("frame_id", std::int64_t{100 + index});
    items.push_back(
        {"camera-" + std::to_string(index), std::move(packet), static_cast<std::size_t>(index)});
  }
  PacketBatch input(std::move(items), Timestamp::FromMicroseconds(1));
  NodeContext process = MakeContext(
      {{"batch", Packet::Make(std::move(input), Timestamp::FromMicroseconds(1))}}, &output);
  ASSERT_TRUE(node->Process(process).ok());
  ASSERT_TRUE(output.Is<PacketBatch>());
  ASSERT_EQ(output.Get<PacketBatch>().size(), 2U);
  EXPECT_EQ(raw->infer_count, 2);
  EXPECT_EQ(output.Get<PacketBatch>().items()[1].source_id, "camera-1");
  EXPECT_EQ(output.Get<PacketBatch>().items()[1].packet.metadata().Get<std::int64_t>("frame_id"),
            101);
}

TEST(AudioNodeTest, RecordsAndRecoversOverrunWithoutHardware) {
  auto backend = std::make_unique<RecoveringAlsa>();
  RecoveringAlsa* raw = backend.get();
  auto node = MakeAlsaCaptureNode(std::move(backend));
  ASSERT_TRUE(node->Configure({{"device", "fake"},
                               {"sample_rate", 16000},
                               {"channels", 1},
                               {"format", "s16_le"},
                               {"frame_ms", 20}})
                  .ok());
  ASSERT_TRUE(node->Open().ok());
  MetricsRegistry metrics;
  std::atomic<bool> cancelled{false};
  NodeContext context(
      "audio", "io", {}, [](const std::string&, Packet) { return Status::Ok(); }, nullptr, nullptr,
      nullptr, &metrics, &cancelled);
  ASSERT_TRUE(node->Start(context).ok());
  ASSERT_TRUE(raw->WaitForRecovery());
  EXPECT_EQ(metrics.Counter("alsa.overruns"), 1U);
  EXPECT_TRUE(node->Stop().ok());
  EXPECT_EQ(metrics.Counter("alsa.overruns"), 1U);
  EXPECT_EQ(metrics.Counter("alsa.capture_errors"), 0U);
  EXPECT_TRUE(node->Close().ok());
}

TEST(StreamingNodeTest, StopCancelsBlockingInputRead) {
  auto backend = std::make_unique<BlockingStreamingInput>();
  BlockingStreamingInput* raw = backend.get();
  auto node = MakeStreamingInputNode(std::move(backend));
  ASSERT_TRUE(node->Configure({{"url", "rtsp://example.invalid/stream"}}).ok());
  ASSERT_TRUE(node->Open().ok());
  MetricsRegistry metrics;
  std::atomic<bool> cancelled{false};
  NodeContext context(
      "input", "network", {}, [](const std::string&, Packet) { return Status::Ok(); }, nullptr,
      nullptr, nullptr, &metrics, &cancelled);
  ASSERT_TRUE(node->Start(context).ok());
  ASSERT_TRUE(raw->WaitUntilReading());
  const auto started = std::chrono::steady_clock::now();
  EXPECT_TRUE(node->Stop().ok());
  EXPECT_LT(std::chrono::steady_clock::now() - started, std::chrono::seconds(1));
  EXPECT_TRUE(node->Close().ok());
}

TEST(OpenClNodeTest, CompilesGenericKernelAndPropagatesFence) {
  auto backend = std::make_unique<RecordingOpenCl>();
  RecordingOpenCl* raw = backend.get();
  auto node = MakeOpenClKernelNode(std::move(backend));
  NodeOptions options;
  options["kernel"] = "copy_bytes";
  options["source"] = "__kernel void copy_bytes(__global uchar* in, __global uchar* out) {}";
  options["global_work_size"] = ConfigValue::Array{ConfigValue(64)};
  options["output_sizes"] = ConfigValue::Array{ConfigValue(64), ConfigValue(16)};
  ASSERT_TRUE(node->Configure(options).ok());
  ASSERT_TRUE(node->Open().ok());
  EXPECT_EQ(raw->compiled.name, "copy_bytes");

  Tensor input;
  input.desc.name = "input";
  input.desc.byte_size = 64;
  input.buffer = std::make_shared<HostBuffer>(64);
  Packet output;
  NodeContext start = MakeContext({}, &output);
  ASSERT_TRUE(node->Start(start).ok());
  NodeContext process = MakeContext(
      {{"tensors", Packet::Make(TensorSet{input}, Timestamp::FromMicroseconds(7))}}, &output);
  ASSERT_TRUE(node->Process(process).ok());
  ASSERT_TRUE(output.Is<TensorSet>());
  ASSERT_EQ(output.Get<TensorSet>().size(), 2U);
  EXPECT_EQ(raw->input_count, 1U);
  EXPECT_EQ(raw->output_count, 2U);
  EXPECT_EQ(raw->global_work_size, std::vector<std::size_t>({64}));
  EXPECT_EQ(output.Get<TensorSet>()[0].buffer->fence(), raw->fence);
  EXPECT_EQ(output.Get<TensorSet>()[1].buffer->fence(), raw->fence);
}

}  // namespace
}  // namespace rkavp
