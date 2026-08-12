#include <gtest/gtest.h>

#include <chrono>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "rkavp/core.hpp"

namespace rkavp {
namespace {

struct Results {
  std::mutex mutex;
  std::condition_variable changed;
  std::vector<PacketSet> invocations;
};

class FanoutNode final : public Node {
 public:
  NodeContract Contract() const override {
    return {{{"in", MediaCaps::Any(), true}},
            {{"fast", MediaCaps::Any(), true}, {"slow", MediaCaps::Any(), true}},
            {},
            InputPolicy::kAny,
            {}};
  }

 protected:
  Status OnProcess(NodeContext& context) override {
    const Packet* packet = context.Input("in");
    if (packet == nullptr) return Status::Invalid("missing input");
    Status status = context.Emit("fast", *packet);
    return status.ok() ? context.Emit("slow", *packet) : status;
  }
};

class RecordingNode final : public Node {
 public:
  RecordingNode(std::shared_ptr<Results> results, InputPolicy policy, bool slow = false)
      : results_(std::move(results)), policy_(policy), slow_(slow) {}
  NodeContract Contract() const override {
    if (policy_ == InputPolicy::kAny)
      return {{{"in", MediaCaps::Any(), true}}, {}, {}, policy_, {}};
    return {{{"a", MediaCaps::Any(), true}, {"b", MediaCaps::Any(), true}}, {}, {}, policy_, "a"};
  }

 protected:
  Status OnProcess(NodeContext& context) override {
    if (slow_) std::this_thread::sleep_for(std::chrono::milliseconds(40));
    {
      std::lock_guard<std::mutex> lock(results_->mutex);
      results_->invocations.push_back(context.inputs());
    }
    results_->changed.notify_all();
    return Status::Ok();
  }

 private:
  std::shared_ptr<Results> results_;
  InputPolicy policy_;
  bool slow_;
};

TEST(GraphRunnerTest, UsesNamedExecutorsAndIsolatesSlowEdges) {
  auto fast = std::make_shared<Results>();
  auto slow = std::make_shared<Results>();
  NodeRegistry registry;
  ASSERT_TRUE(registry.Register("Fanout", [] { return std::make_unique<FanoutNode>(); }).ok());
  ASSERT_TRUE(
      registry
          .Register("Fast",
                    [fast] { return std::make_unique<RecordingNode>(fast, InputPolicy::kAny); })
          .ok());
  ASSERT_TRUE(registry
                  .Register("Slow",
                            [slow] {
                              return std::make_unique<RecordingNode>(slow, InputPolicy::kAny, true);
                            })
                  .ok());
  GraphConfig config;
  config.name = "branch-isolation";
  config.executors = {{"source", 1, 32, 0}, {"fast", 1, 32, 0}, {"slow", 1, 32, 0}};
  config.nodes = {
      {"source", "Fanout", "source", {}}, {"main", "Fast", "fast", {}}, {"ai", "Slow", "slow", {}}};
  config.inputs = {{"input", "source", "in"}};
  config.edges = {{"source", "fast", "main", "in", {8, QueuePolicy::kBlock}},
                  {"source", "slow", "ai", "in", {1, QueuePolicy::kDropOldest}}};
  GraphRunner runner(Graph(config, &registry), &registry);
  ASSERT_TRUE(runner.Start().ok());
  for (int i = 0; i < 5; ++i)
    ASSERT_TRUE(runner.AddPacket("input", Packet::Make(i, Timestamp::FromMicroseconds(i))).ok());
  std::unique_lock<std::mutex> lock(fast->mutex);
  ASSERT_TRUE(fast->changed.wait_for(lock, std::chrono::seconds(1),
                                     [&] { return fast->invocations.size() == 5; }));
  lock.unlock();
  EXPECT_GT(runner.metrics().Counter("edge.source.slow_to_ai.in.dropped"), 0U);
  runner.CloseInputStream("input");
  EXPECT_TRUE(runner.WaitUntilDone(1000).ok());
  runner.Stop();
}

TEST(GraphRunnerTest, SynchronizesPacketsByTimestamp) {
  auto results = std::make_shared<Results>();
  NodeRegistry registry;
  ASSERT_TRUE(registry
                  .Register("Sync",
                            [results] {
                              return std::make_unique<RecordingNode>(results, InputPolicy::kSync);
                            })
                  .ok());
  GraphConfig config;
  config.name = "sync";
  config.nodes = {{"join", "Sync", "default", {}}};
  config.inputs = {{"a", "join", "a"}, {"b", "join", "b"}};
  GraphRunner runner(Graph(config, &registry), &registry);
  ASSERT_TRUE(runner.Start().ok());
  ASSERT_TRUE(runner.AddPacket("a", Packet::Make(1, Timestamp::FromMicroseconds(10))).ok());
  ASSERT_TRUE(runner.AddPacket("b", Packet::Make(3, Timestamp::FromMicroseconds(10))).ok());
  ASSERT_TRUE(runner.AddPacket("b", Packet::Make(2, Timestamp::FromMicroseconds(20))).ok());
  EXPECT_FALSE(runner.AddPacket("b", Packet::Make(4, Timestamp::FromMicroseconds(15))).ok());
  ASSERT_TRUE(runner.WaitUntilIdle(1000).ok());
  std::lock_guard<std::mutex> lock(results->mutex);
  ASSERT_EQ(results->invocations.size(), 1U);
  EXPECT_EQ(results->invocations[0].at("a").timestamp(), Timestamp::FromMicroseconds(10));
  runner.Stop();
}

TEST(GraphRunnerTest, TimestampBoundsDiscardPacketsThatCanNoLongerMatch) {
  auto results = std::make_shared<Results>();
  NodeRegistry registry;
  ASSERT_TRUE(registry
                  .Register("Sync",
                            [results] {
                              return std::make_unique<RecordingNode>(results, InputPolicy::kSync);
                            })
                  .ok());
  GraphConfig config;
  config.name = "sync-bounds";
  config.nodes = {{"join", "Sync", "default", {}}};
  config.inputs = {{"a", "join", "a"}, {"b", "join", "b"}};
  GraphRunner runner(Graph(config, &registry), &registry);
  ASSERT_TRUE(runner.Start().ok());
  ASSERT_TRUE(runner.AddPacket("a", Packet::Make(1, Timestamp::FromMicroseconds(10))).ok());
  ASSERT_TRUE(runner.SetInputTimestampBound("b", Timestamp::FromMicroseconds(11)).ok());
  EXPECT_FALSE(runner.AddPacket("b", Packet::Make(2, Timestamp::FromMicroseconds(10))).ok());
  ASSERT_TRUE(runner.WaitUntilIdle(1000).ok());
  EXPECT_EQ(runner.metrics().Counter("graph.sync_unmatched_dropped"), 1U);
  std::lock_guard<std::mutex> lock(results->mutex);
  EXPECT_TRUE(results->invocations.empty());
  runner.Stop();
}

TEST(GraphRunnerTest, LatestPolicyUsesAuxiliaryStateAndObservesOutput) {
  class LatestNode final : public Node {
   public:
    NodeContract Contract() const override {
      return {{{"trigger", MediaCaps::Any(), true}, {"state", MediaCaps::Any(), true}},
              {{"out", MediaCaps::Any(), true}},
              {},
              InputPolicy::kLatest,
              "trigger"};
    }

   protected:
    Status OnProcess(NodeContext& context) override {
      return context.Emit("out", Packet::Make(context.Input("trigger")->Get<int>() +
                                                  context.Input("state")->Get<int>(),
                                              context.Input("trigger")->timestamp()));
    }
  };
  NodeRegistry registry;
  ASSERT_TRUE(registry.Register("Latest", [] { return std::make_unique<LatestNode>(); }).ok());
  GraphConfig config;
  config.name = "latest";
  config.nodes = {{"latest", "Latest", "default", {}}};
  config.inputs = {{"trigger", "latest", "trigger"}, {"state", "latest", "state"}};
  config.outputs = {{"result", "latest", "out"}};
  GraphRunner runner(Graph(config, &registry), &registry);
  int observed = 0;
  ASSERT_TRUE(
      runner.ObserveOutput("result", [&](const Packet& packet) { observed = packet.Get<int>(); })
          .ok());
  ASSERT_TRUE(runner.Start().ok());
  ASSERT_TRUE(runner.AddPacket("state", Packet::Make(7, Timestamp::FromMicroseconds(1))).ok());
  ASSERT_TRUE(runner.AddPacket("trigger", Packet::Make(5, Timestamp::FromMicroseconds(2))).ok());
  ASSERT_TRUE(runner.WaitUntilIdle(1000).ok());
  EXPECT_EQ(observed, 12);
  EXPECT_FALSE(runner.trace().Snapshot().empty());
  runner.Stop();
}

TEST(GraphRunnerTest, CanStartAndStopRepeatedlyWithSidePackets) {
  NodeRegistry registry;
  RegisterBuiltinNodes(&registry);
  GraphConfig config;
  config.name = "restart";
  config.nodes = {{"pass", "Passthrough", "default", {}}};
  config.inputs = {{"input", "pass", "in"}};
  config.outputs = {{"output", "pass", "out"}};
  config.side_packets = {{"session", ConfigValue("stable")}};
  GraphRunner runner(Graph(std::move(config), &registry), &registry);

  for (int iteration = 0; iteration < 2; ++iteration) {
    ASSERT_TRUE(runner.Start().ok());
    ASSERT_TRUE(
        runner.AddPacket("input", Packet::Make(iteration, Timestamp::FromMicroseconds(iteration)))
            .ok());
    ASSERT_TRUE(runner.WaitUntilIdle(1000).ok());
    runner.Stop();
    EXPECT_FALSE(runner.running());
  }
}

}  // namespace
}  // namespace rkavp
