#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <future>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

#include "rkavp/core.hpp"

namespace rkavp {
namespace {

class StartEmitterNode final : public Node {
 public:
  NodeContract Contract() const override {
    return {{}, {{"out", MediaCaps::Any(), true}}, {}, InputPolicy::kAny, {}};
  }

 protected:
  Status OnConfigure(const NodeOptions& options) override {
    std::int64_t value = 0;
    Status status = GetIntegerOption(options, "value", &value);
    if (status.ok()) value_ = static_cast<int>(value);
    return status;
  }
  Status OnStart(NodeContext& context) override {
    return context.Emit("out", Packet::Make(value_, Timestamp::FromMicroseconds(1)));
  }
  Status OnProcess(NodeContext&) override { return Status::Ok(); }

 private:
  int value_ = 0;
};

class LifecycleProbeNode final : public Node {
 public:
  LifecycleProbeNode(std::string name, std::shared_ptr<std::vector<std::string>> events,
                     bool fail_start)
      : name_(std::move(name)), events_(std::move(events)), fail_start_(fail_start) {}
  NodeContract Contract() const override { return {}; }

 protected:
  Status OnConfigure(const NodeOptions&) override {
    events_->push_back(name_ + ".configure");
    return Status::Ok();
  }
  Status OnOpen() override {
    events_->push_back(name_ + ".open");
    return Status::Ok();
  }
  Status OnStart(NodeContext&) override {
    events_->push_back(name_ + ".start");
    return fail_start_ ? Status::Internal("requested start failure") : Status::Ok();
  }
  Status OnProcess(NodeContext&) override { return Status::Ok(); }
  Status OnStop() override {
    events_->push_back(name_ + ".stop");
    return Status::Ok();
  }
  Status OnClose() override {
    events_->push_back(name_ + ".close");
    return Status::Ok();
  }

 private:
  std::string name_;
  std::shared_ptr<std::vector<std::string>> events_;
  bool fail_start_;
};

struct BlockingLifecycleState {
  std::promise<void> entered_promise;
  std::shared_future<void> entered = entered_promise.get_future().share();
  std::promise<void> release_promise;
  std::shared_future<void> release = release_promise.get_future().share();
  std::atomic<bool> entered_once{false};
  std::atomic<bool> processing{false};
  std::atomic<bool> stop_requested{false};
  std::atomic<bool> stop_called{false};
  std::atomic<bool> stop_during_process{false};
};

class BlockingLifecycleNode final : public Node {
 public:
  explicit BlockingLifecycleNode(std::shared_ptr<BlockingLifecycleState> state)
      : state_(std::move(state)) {}
  NodeContract Contract() const override {
    return {{{"in", MediaCaps::Any(), true}}, {}, {}, InputPolicy::kAny, {}};
  }

 protected:
  Status OnProcess(NodeContext&) override {
    state_->processing = true;
    if (!state_->entered_once.exchange(true)) state_->entered_promise.set_value();
    state_->release.wait();
    state_->processing = false;
    return Status::Ok();
  }
  void OnRequestStop() override { state_->stop_requested = true; }
  Status OnStop() override {
    state_->stop_called = true;
    state_->stop_during_process = state_->processing.load();
    return Status::Ok();
  }

 private:
  std::shared_ptr<BlockingLifecycleState> state_;
};

class FailingProcessNode final : public Node {
 public:
  NodeContract Contract() const override {
    return {{{"in", MediaCaps::Any(), true}}, {}, {}, InputPolicy::kAny, {}};
  }

 protected:
  Status OnProcess(NodeContext&) override { return Status::Internal("requested process failure"); }
};

GraphConfig PassthroughGraph(std::string name = "runtime-features") {
  GraphConfig config;
  config.version = 2;
  config.name = std::move(name);
  config.nodes = {{"pass", "Passthrough", "default", {}}};
  config.inputs = {{"input", "pass", "in", {32, QueuePolicy::kBlock}}};
  config.outputs = {{"output", "pass", "out"}};
  return config;
}

TEST(RuntimeFeatureTest, ObserverQueueIsIsolatedAndCanBeCancelled) {
  NodeRegistry registry;
  RegisterBuiltinNodes(&registry);
  GraphRunner runner(Graph(PassthroughGraph(), &registry), &registry);
  std::promise<void> entered;
  std::shared_future<void> release = std::async(std::launch::deferred, [] {}).share();
  std::promise<void> release_promise;
  release = release_promise.get_future().share();
  std::atomic<int> callbacks{0};
  ObserverHandle handle;
  ObserverOptions options;
  options.queue_capacity = 1;
  options.queue_policy = QueuePolicy::kDropOldest;
  ASSERT_TRUE(runner
                  .ObserveOutput(
                      "output",
                      [&](const Packet&) {
                        if (++callbacks == 1) entered.set_value();
                        release.wait();
                      },
                      options, &handle)
                  .ok());
  ASSERT_TRUE(runner.Start().ok());
  ASSERT_TRUE(runner.AddPacket("input", Packet::Make(0, Timestamp::FromMicroseconds(0))).ok());
  ASSERT_EQ(entered.get_future().wait_for(std::chrono::seconds(1)), std::future_status::ready);
  for (int i = 1; i < 10; ++i) {
    ASSERT_TRUE(runner.AddPacket("input", Packet::Make(i, Timestamp::FromMicroseconds(i))).ok());
  }
  const auto info = runner.GetRuntimeInfo();
  EXPECT_TRUE(info.running);
  ASSERT_FALSE(info.executors.empty());
  release_promise.set_value();
  EXPECT_TRUE(runner.WaitForObservedOutput(handle, 1000).ok());
  EXPECT_TRUE(runner.WaitUntilIdle(1000).ok());
  EXPECT_TRUE(runner.CancelObserver(handle).ok());
  runner.Stop();
}

TEST(RuntimeFeatureTest, RejectsBlockingObserverQueue) {
  NodeRegistry registry;
  RegisterBuiltinNodes(&registry);
  GraphRunner runner(Graph(PassthroughGraph(), &registry), &registry);
  ObserverHandle handle;
  ObserverOptions options;
  options.queue_capacity = 1;
  options.queue_policy = QueuePolicy::kBlock;
  const Status status = runner.ObserveOutput(
      "output", [](const Packet&) {}, options, &handle);
  EXPECT_EQ(status.code(), StatusCode::kInvalidArgument);
  EXPECT_NE(status.message().find("cannot be block"), std::string::npos);
}

TEST(RuntimeFeatureTest, ObserverCanCancelItselfWithoutLeakingAJoinableThread) {
  NodeRegistry registry;
  RegisterBuiltinNodes(&registry);
  GraphRunner runner(Graph(PassthroughGraph(), &registry), &registry);
  ObserverHandle handle;
  std::promise<Status> cancelled;
  ASSERT_TRUE(runner
                  .ObserveOutput(
                      "output",
                      [&](const Packet&) { cancelled.set_value(runner.CancelObserver(handle)); },
                      {}, &handle)
                  .ok());
  ASSERT_TRUE(runner.Start().ok());
  ASSERT_TRUE(runner.AddPacket("input", Packet::Make(1, Timestamp::FromMicroseconds(1))).ok());
  auto result = cancelled.get_future();
  ASSERT_EQ(result.wait_for(std::chrono::seconds(1)), std::future_status::ready);
  EXPECT_TRUE(result.get().ok());
  runner.Stop();
}

TEST(RuntimeFeatureTest, StopRequestsCancellationBeforeCallingNodeStop) {
  auto state = std::make_shared<BlockingLifecycleState>();
  NodeRegistry registry;
  ASSERT_TRUE(registry
                  .Register("BlockingLifecycle",
                            [state] { return std::make_unique<BlockingLifecycleNode>(state); })
                  .ok());
  GraphConfig config;
  config.version = 2;
  config.name = "blocking-lifecycle";
  config.nodes = {{"block", "BlockingLifecycle", "default", {}}};
  config.inputs = {{"input", "block", "in", {2, QueuePolicy::kBlock}}};
  GraphRunner runner(Graph(config, &registry), &registry);
  ASSERT_TRUE(runner.Start().ok());
  ASSERT_TRUE(runner.AddPacket("input", Packet::Make(1, Timestamp::FromMicroseconds(1))).ok());
  ASSERT_EQ(state->entered.wait_for(std::chrono::seconds(1)), std::future_status::ready);
  auto stopped = std::async(std::launch::async, [&runner] { runner.Stop(); });
  for (int i = 0; i < 100 && !state->stop_requested; ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  EXPECT_TRUE(state->stop_requested);
  EXPECT_FALSE(state->stop_called);
  state->release_promise.set_value();
  ASSERT_EQ(stopped.wait_for(std::chrono::seconds(1)), std::future_status::ready);
  EXPECT_TRUE(state->stop_called);
  EXPECT_FALSE(state->stop_during_process);
}

TEST(RuntimeFeatureTest, ErrorCallbackCanStopRunnerWithoutJoiningExecutorThread) {
  NodeRegistry registry;
  ASSERT_TRUE(
      registry.Register("FailingProcess", [] { return std::make_unique<FailingProcessNode>(); })
          .ok());
  GraphConfig config;
  config.version = 2;
  config.name = "error-callback-stop";
  config.nodes = {{"fail", "FailingProcess", "default", {}}};
  config.inputs = {{"input", "fail", "in", {2, QueuePolicy::kBlock}}};
  GraphRunner runner(Graph(config, &registry), &registry);
  std::promise<void> stopped;
  ASSERT_TRUE(runner
                  .SetErrorCallback([&](const Status&) {
                    runner.Stop();
                    stopped.set_value();
                  })
                  .ok());
  ASSERT_TRUE(runner.Start().ok());
  ASSERT_TRUE(runner.AddPacket("input", Packet::Make(1, Timestamp::FromMicroseconds(1))).ok());
  EXPECT_EQ(stopped.get_future().wait_for(std::chrono::seconds(1)), std::future_status::ready);
  EXPECT_FALSE(runner.running());
}

TEST(RuntimeFeatureTest, PropagatesEosToOutputObserver) {
  NodeRegistry registry;
  RegisterBuiltinNodes(&registry);
  GraphRunner runner(Graph(PassthroughGraph("eos"), &registry), &registry);
  std::atomic<bool> eos{false};
  ObserverHandle handle;
  ASSERT_TRUE(runner
                  .ObserveOutput(
                      "output",
                      [&](const Packet& packet) {
                        if (packet.event() == ControlEvent::kEndOfStream) eos = true;
                      },
                      {}, &handle)
                  .ok());
  ASSERT_TRUE(runner.Start().ok());
  ASSERT_TRUE(runner.CloseInputStream("input").ok());
  ASSERT_TRUE(runner.WaitUntilDone(1000).ok());
  ASSERT_TRUE(runner.WaitForObservedOutput(handle, 1000).ok());
  EXPECT_TRUE(eos);
  runner.Stop();
}

TEST(RuntimeFeatureTest, FlowLimiterReleasesNewestQueuedPacketOnFeedback) {
  NodeRegistry registry;
  RegisterBuiltinNodes(&registry);
  GraphConfig config;
  config.version = 2;
  config.name = "flow-limiter";
  config.nodes = {{"limit",
                   "FlowLimiter",
                   "default",
                   {{"max_in_flight", ConfigValue(std::int64_t{1})},
                    {"queue_capacity", ConfigValue(std::int64_t{1})},
                    {"queue_policy", ConfigValue("drop_oldest")}}}};
  config.inputs = {{"input", "limit", "in"}, {"finished", "limit", "finished"}};
  config.outputs = {{"output", "limit", "out"}};
  GraphRunner runner(Graph(config, &registry), &registry);
  std::mutex mutex;
  std::vector<int> values;
  ObserverHandle handle;
  ASSERT_TRUE(runner
                  .ObserveOutput(
                      "output",
                      [&](const Packet& packet) {
                        if (packet.event() == ControlEvent::kNone) {
                          std::lock_guard<std::mutex> lock(mutex);
                          values.push_back(packet.Get<int>());
                        }
                      },
                      {}, &handle)
                  .ok());
  ASSERT_TRUE(runner.Start().ok());
  for (int i = 1; i <= 3; ++i) {
    ASSERT_TRUE(runner.AddPacket("input", Packet::Make(i, Timestamp::FromMicroseconds(i))).ok());
  }
  ASSERT_TRUE(runner.WaitUntilIdle(1000).ok());
  ASSERT_TRUE(
      runner.AddPacket("finished", Packet::Make(true, Timestamp::FromMicroseconds(4))).ok());
  ASSERT_TRUE(runner.WaitUntilIdle(1000).ok());
  std::lock_guard<std::mutex> lock(mutex);
  ASSERT_EQ(values.size(), 2U);
  EXPECT_EQ(values[0], 1);
  EXPECT_EQ(values[1], 3);
  runner.Stop();
}

TEST(RuntimeFeatureTest, AdaptiveBatchUsesFakeClockForPartialTimeout) {
  NodeRegistry registry;
  RegisterBuiltinNodes(&registry);
  GraphConfig config;
  config.version = 2;
  config.name = "adaptive-batch";
  config.nodes = {{"mux",
                   "AdaptiveBatch",
                   "default",
                   {{"max_batch_size", ConfigValue(std::int64_t{4})},
                    {"max_per_source", ConfigValue(std::int64_t{2})},
                    {"timeout_us", ConfigValue(std::int64_t{1000000})}}}};
  config.inputs = {{"input", "mux", "in"}};
  config.outputs = {{"batch", "mux", "batch"}};
  GraphRunner runner(Graph(config, &registry), &registry);
  auto clock = std::make_shared<FakeClock>();
  runner.SetClock(clock);
  std::vector<PacketBatch> batches;
  std::mutex mutex;
  ObserverHandle handle;
  ASSERT_TRUE(runner
                  .ObserveOutput(
                      "batch",
                      [&](const Packet& packet) {
                        if (packet.Is<PacketBatch>()) {
                          std::lock_guard<std::mutex> lock(mutex);
                          batches.push_back(packet.Get<PacketBatch>());
                        }
                      },
                      {}, &handle)
                  .ok());
  ASSERT_TRUE(runner.Start().ok());
  Packet first = Packet::Make(1, Timestamp::FromMicroseconds(1));
  first.mutable_metadata().Set("source_id", std::string("camera-a"));
  ASSERT_TRUE(runner.AddPacket("input", std::move(first)).ok());
  ASSERT_TRUE(runner.WaitUntilIdle(1000).ok());
  clock->AdvanceMicros(1000001);
  Packet second = Packet::Make(2, Timestamp::FromMicroseconds(2));
  second.mutable_metadata().Set("source_id", std::string("camera-b"));
  ASSERT_TRUE(runner.AddPacket("input", std::move(second)).ok());
  ASSERT_TRUE(runner.WaitForObservedOutput(handle, 1000).ok());
  {
    std::lock_guard<std::mutex> lock(mutex);
    ASSERT_EQ(batches.size(), 1U);
    ASSERT_EQ(batches[0].size(), 1U);
    EXPECT_EQ(batches[0].items()[0].source_id, "camera-a");
  }
  runner.Stop();
}

TEST(RuntimeFeatureTest, EncodedPacketRingBufferCollectsPreAndPostEncodedPackets) {
  NodeRegistry registry;
  RegisterBuiltinNodes(&registry);
  GraphConfig config;
  config.version = 2;
  config.name = "smart-record";
  config.nodes = {{"record",
                   "EncodedPacketRingBuffer",
                   "default",
                   {{"capacity", ConfigValue(std::int64_t{2})},
                    {"post_packets", ConfigValue(std::int64_t{2})}}}};
  config.inputs = {{"packet", "record", "packet"}, {"trigger", "record", "trigger"}};
  config.outputs = {{"clip", "record", "clip"}};
  GraphRunner runner(Graph(config, &registry), &registry);
  PacketBatch clip;
  ObserverHandle handle;
  ASSERT_TRUE(runner
                  .ObserveOutput(
                      "clip",
                      [&](const Packet& packet) {
                        if (packet.Is<PacketBatch>()) clip = packet.Get<PacketBatch>();
                      },
                      {}, &handle)
                  .ok());
  ASSERT_TRUE(runner.Start().ok());
  const auto add_encoded = [&](std::int64_t pts) {
    EncodedPacket encoded;
    encoded.pts = Timestamp::FromMicroseconds(pts);
    return runner.AddPacket("packet",
                            Packet::Make(std::move(encoded), Timestamp::FromMicroseconds(pts)));
  };
  ASSERT_TRUE(add_encoded(1).ok());
  ASSERT_TRUE(add_encoded(2).ok());
  ASSERT_TRUE(runner.WaitUntilIdle(1000).ok());
  ASSERT_TRUE(runner.AddPacket("trigger", Packet::Make(true, Timestamp::FromMicroseconds(2))).ok());
  ASSERT_TRUE(runner.WaitUntilIdle(1000).ok());
  ASSERT_TRUE(add_encoded(3).ok());
  ASSERT_TRUE(add_encoded(4).ok());
  ASSERT_TRUE(runner.WaitForObservedOutput(handle, 1000).ok());
  EXPECT_EQ(clip.size(), 4U);
  runner.Stop();
}

TEST(RuntimeFeatureTest, ManagesPredeclaredDynamicSourceSlots) {
  NodeRegistry registry;
  RegisterBuiltinNodes(&registry);
  GraphConfig config = PassthroughGraph("sources");
  config.source_slots = {{"camera", "input"}};
  GraphRunner runner(Graph(config, &registry), &registry);
  ASSERT_TRUE(runner.Start().ok());
  for (int i = 0; i < 100; ++i) {
    const std::string source_id = "camera-" + std::to_string(i);
    ASSERT_TRUE(runner.AddSource("camera", source_id).ok());
    ASSERT_TRUE(
        runner.AddSourcePacket(source_id, Packet::Make(i, Timestamp::FromMicroseconds(i))).ok());
    ASSERT_EQ(runner.GetSourceHealth(source_id).state, StreamHealthState::kStreaming);
    ASSERT_TRUE(runner.RemoveSource(source_id, 1000).ok());
  }
  EXPECT_TRUE(runner.ListSources().empty());
  runner.Stop();
}

TEST(RuntimeFeatureTest, RemovingIdleSourceDoesNotWaitForAnotherSource) {
  NodeRegistry registry;
  RegisterBuiltinNodes(&registry);
  GraphConfig config = PassthroughGraph("source-drain-isolation");
  config.source_slots = {{"camera", "input"}};
  GraphRunner runner(Graph(config, &registry), &registry);
  std::promise<void> observer_entered;
  std::promise<void> observer_release;
  auto release = observer_release.get_future().share();
  ObserverHandle handle;
  ASSERT_TRUE(runner
                  .ObserveOutput(
                      "output",
                      [&](const Packet&) {
                        observer_entered.set_value();
                        release.wait();
                      },
                      {}, &handle)
                  .ok());
  ASSERT_TRUE(runner.Start().ok());
  ASSERT_TRUE(runner.AddSource("camera", "busy").ok());
  ASSERT_TRUE(runner.AddSource("camera", "idle").ok());
  ASSERT_TRUE(runner.AddSourcePacket("busy", Packet::Make(1, Timestamp::FromMicroseconds(1))).ok());
  ASSERT_EQ(observer_entered.get_future().wait_for(std::chrono::seconds(1)),
            std::future_status::ready);
  EXPECT_TRUE(runner.RemoveSource("idle", 50).ok());
  EXPECT_EQ(runner.GetSourceHealth("busy").state, StreamHealthState::kStreaming);
  observer_release.set_value();
  EXPECT_TRUE(runner.RemoveSource("busy", 1000).ok());
  runner.Stop();
}

TEST(RuntimeFeatureTest, WaitUntilDoneWaitsForActiveDynamicSources) {
  NodeRegistry registry;
  RegisterBuiltinNodes(&registry);
  GraphConfig config = PassthroughGraph("source-done");
  config.source_slots = {{"camera", "input"}};
  GraphRunner runner(Graph(config, &registry), &registry);
  ASSERT_TRUE(runner.Start().ok());
  ASSERT_TRUE(runner.AddSource("camera", "front").ok());
  ASSERT_TRUE(runner.CloseInputStream("input").ok());
  EXPECT_EQ(runner.WaitUntilDone(20).code(), StatusCode::kUnavailable);
  ASSERT_TRUE(runner.RemoveSource("front", 1000).ok());
  EXPECT_TRUE(runner.WaitUntilDone(1000).ok());
  runner.Stop();
}

TEST(RuntimeFeatureTest, SourceCanRestartAfterDrainTimeout) {
  NodeRegistry registry;
  RegisterBuiltinNodes(&registry);
  GraphConfig config = PassthroughGraph("source-restart-after-timeout");
  config.source_slots = {{"camera", "input"}};
  GraphRunner runner(Graph(config, &registry), &registry);
  std::promise<void> observer_entered;
  std::promise<void> observer_release;
  auto release = observer_release.get_future().share();
  ObserverHandle handle;
  ASSERT_TRUE(runner
                  .ObserveOutput(
                      "output",
                      [&](const Packet&) {
                        observer_entered.set_value();
                        release.wait();
                      },
                      {}, &handle)
                  .ok());
  ASSERT_TRUE(runner.Start().ok());
  ASSERT_TRUE(runner.AddSource("camera", "front").ok());
  ASSERT_TRUE(
      runner.AddSourcePacket("front", Packet::Make(1, Timestamp::FromMicroseconds(1))).ok());
  ASSERT_EQ(observer_entered.get_future().wait_for(std::chrono::seconds(1)),
            std::future_status::ready);
  EXPECT_EQ(runner.RemoveSource("front", 10).code(), StatusCode::kUnavailable);
  observer_release.set_value();
  ASSERT_TRUE(runner.WaitUntilIdle(1000).ok());
  ASSERT_TRUE(runner.RestartSource("front").ok());
  EXPECT_TRUE(
      runner.AddSourcePacket("front", Packet::Make(2, Timestamp::FromMicroseconds(2))).ok());
  ASSERT_TRUE(runner.RemoveSource("front", 1000).ok());
  runner.Stop();
}

TEST(RuntimeFeatureTest, StartsSourceSubgraphTemplateAndBridgesItsOutput) {
  const auto path =
      std::filesystem::temp_directory_path() /
      ("rkavp-source-template-" +
       std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + ".yaml");
  {
    std::ofstream file(path);
    file << "version: 2\n"
            "graph:\n"
            "  name: source-template\n"
            "  outputs: {packet: emit.out}\n"
            "  nodes:\n"
            "    - id: emit\n"
            "      type: StartEmitter\n"
            "      options: {value: 1}\n";
  }
  NodeRegistry registry;
  RegisterBuiltinNodes(&registry);
  ASSERT_TRUE(
      registry.Register("StartEmitter", [] { return std::make_unique<StartEmitterNode>(); }).ok());
  GraphConfig config = PassthroughGraph("source-subgraph");
  config.source_slots = {{"camera", "input", path.string(), "packet"}};
  GraphRunner runner(Graph(config, &registry), &registry);
  std::atomic<int> observed{0};
  ObserverHandle handle;
  ASSERT_TRUE(runner
                  .ObserveOutput(
                      "output",
                      [&](const Packet& packet) {
                        if (packet.event() == ControlEvent::kNone) observed = packet.Get<int>();
                      },
                      {}, &handle)
                  .ok());
  ASSERT_TRUE(runner.Start().ok());
  ConfigValue::Object options{{"emit.value", ConfigValue(std::int64_t{42})}};
  ASSERT_TRUE(runner.AddSource("camera", "front", ConfigValue(std::move(options))).ok());
  ASSERT_TRUE(runner.WaitForObservedOutput(handle, 1000).ok());
  EXPECT_EQ(observed.load(), 42);
  ASSERT_TRUE(runner.RemoveSource("front", 1000).ok());
  runner.Stop();
  std::error_code error;
  std::filesystem::remove(path, error);
}

TEST(RuntimeFeatureTest, RollsBackFailedStartupInReverseOrder) {
  auto events = std::make_shared<std::vector<std::string>>();
  NodeRegistry registry;
  ASSERT_TRUE(registry
                  .Register("First",
                            [events] {
                              return std::make_unique<LifecycleProbeNode>("first", events, false);
                            })
                  .ok());
  ASSERT_TRUE(registry
                  .Register("Second",
                            [events] {
                              return std::make_unique<LifecycleProbeNode>("second", events, true);
                            })
                  .ok());
  GraphConfig config;
  config.version = 2;
  config.name = "rollback";
  config.nodes = {{"first", "First", "default", {}}, {"second", "Second", "default", {}}};
  GraphRunner runner(Graph(config, &registry), &registry);
  EXPECT_FALSE(runner.Start().ok());
  EXPECT_FALSE(runner.running());
  const std::vector<std::string> suffix{"second.stop", "first.stop", "second.close", "first.close"};
  ASSERT_GE(events->size(), suffix.size());
  EXPECT_TRUE(std::equal(suffix.begin(), suffix.end(), events->end() - suffix.size()));
}

TEST(StreamingPrimitiveTest, UnwrapsRtpTimestampAndOrdersJitterPackets) {
  RtpTimestampMapper mapper(90000);
  Timestamp first;
  Timestamp wrapped;
  ASSERT_TRUE(mapper.Map(0xfffffff0U, &first).ok());
  ASSERT_TRUE(mapper.Map(0x00000020U, &wrapped).ok());
  EXPECT_EQ(first, Timestamp::FromMicroseconds(0));
  EXPECT_GT(wrapped.microseconds(), 0);

  EncodedJitterBuffer jitter(3, 10);
  EncodedPacket late;
  late.pts = Timestamp::FromMicroseconds(30);
  EncodedPacket early;
  early.pts = Timestamp::FromMicroseconds(10);
  ASSERT_TRUE(jitter.Push(std::move(late)).ok());
  ASSERT_TRUE(jitter.Push(std::move(early)).ok());
  EncodedPacket output;
  ASSERT_TRUE(jitter.PopReady(Timestamp::FromMicroseconds(25), &output));
  EXPECT_EQ(output.pts, Timestamp::FromMicroseconds(10));

  bool dropped = false;
  EncodedPacket too_late;
  too_late.pts = Timestamp::FromMicroseconds(9);
  ASSERT_TRUE(jitter.Push(std::move(too_late), &dropped).ok());
  EXPECT_TRUE(dropped);

  RtcpClockMapper wall_clock(90000);
  ASSERT_TRUE(wall_clock.Update(100, 1000000).ok());
  Timestamp mapped;
  ASSERT_TRUE(wall_clock.Map(90100, &mapped).ok());
  EXPECT_EQ(mapped, Timestamp::FromMicroseconds(2000000));
  ASSERT_TRUE(wall_clock.Update(180100, 2999900).ok());
  EXPECT_EQ(wall_clock.drift_us(), -100);

  ReconnectBackoff backoff(100, 500, 2.0);
  EXPECT_EQ(backoff.NextDelayMs(), 100);
  EXPECT_EQ(backoff.NextDelayMs(), 200);
  EXPECT_EQ(backoff.NextDelayMs(), 400);
  EXPECT_EQ(backoff.NextDelayMs(), 500);
  backoff.Reset();
  EXPECT_EQ(backoff.NextDelayMs(), 100);
}

}  // namespace
}  // namespace rkavp
