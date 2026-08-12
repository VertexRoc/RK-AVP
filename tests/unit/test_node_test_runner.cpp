#include <gtest/gtest.h>

#include <chrono>
#include <future>
#include <memory>
#include <string>
#include <thread>

#include "rkavp/testing/node_test_runner.hpp"

namespace rkavp::testing {
namespace {

class ProbeService {
 public:
  explicit ProbeService(int value) : value(value) {}
  int value;
};

class MemoryResourceManager final : public ResourceManager {
 public:
  Status Read(const std::string& uri, std::string* data) const override {
    if (uri != "resource") return Status::NotFound("missing resource");
    *data = "resource-data";
    return Status::Ok();
  }
};

class InspectNode final : public Node {
 public:
  NodeContract Contract() const override {
    return {{{"in", MediaCaps::Any(), true}}, {{"out", MediaCaps::Any(), true}}, {}};
  }

 protected:
  Status OnConfigure(const NodeOptions& options) override {
    return GetIntegerOption(options, "offset", &offset_);
  }

  Status OnProcess(NodeContext& context) override {
    const Packet* input = context.Input("in");
    const Packet* side = context.SidePacket("bias");
    auto service = context.Service<ProbeService>("probe");
    std::string resource;
    if (input == nullptr || side == nullptr || !service || context.resources() == nullptr) {
      return Status::Invalid("test dependencies are missing");
    }
    Status status = context.resources()->Read("resource", &resource);
    if (!status.ok()) return status;
    context.metrics()->Increment("inspect.processed");
    const int value = input->Get<int>() + side->Get<int>() + service->value +
                      static_cast<int>(offset_) + static_cast<int>(resource.size());
    return context.Emit("out", Packet::Make(value, input->timestamp()));
  }

 private:
  std::int64_t offset_ = 0;
};

class AsyncSourceNode final : public Node {
 public:
  ~AsyncSourceNode() override { StopWorker(); }
  NodeContract Contract() const override { return {{}, {{"out", MediaCaps::Any(), true}}, {}}; }

 protected:
  Status OnStart(NodeContext& context) override {
    worker_ = std::thread([&context] {
      std::this_thread::sleep_for(std::chrono::milliseconds(5));
      if (!context.cancelled()) {
        (void)context.Emit("out", Packet::Make(7, Timestamp::FromMicroseconds(7)));
        (void)context.SetOutputTimestampBound("out", Timestamp::FromMicroseconds(8));
        (void)context.Emit("out", Packet::Event(ControlEvent::kEndOfStream, Timestamp::Done()));
      }
    });
    return Status::Ok();
  }
  Status OnProcess(NodeContext&) override { return Status::Ok(); }
  Status OnStop() override {
    StopWorker();
    return Status::Ok();
  }

 private:
  void StopWorker() {
    if (worker_.joinable()) worker_.join();
  }
  std::thread worker_;
};

class FailingNode final : public Node {
 public:
  explicit FailingNode(std::shared_ptr<int> close_count) : close_count_(std::move(close_count)) {}
  NodeContract Contract() const override { return {}; }

 protected:
  Status OnStart(NodeContext&) override { return Status::Internal("start failed"); }
  Status OnProcess(NodeContext&) override { return Status::Ok(); }
  Status OnClose() override {
    ++*close_count_;
    return Status::Ok();
  }

 private:
  std::shared_ptr<int> close_count_;
};

TEST(NodeTestRunnerTest, InjectsInputsSidePacketsServicesResourcesAndMetrics) {
  NodeTestRunner runner(std::make_unique<InspectNode>());
  ASSERT_TRUE(runner.Configure(ConfigValue::Object{{"offset", ConfigValue(3)}}).ok());
  ASSERT_TRUE(runner.SetInput("in", Packet::Make(1, Timestamp::FromMicroseconds(10))).ok());
  ASSERT_TRUE(runner.SetSidePacket("bias", Packet::Make(2)).ok());
  ASSERT_TRUE(runner.SetService("probe", std::make_shared<ProbeService>(4)).ok());
  runner.SetResourceManager(std::make_shared<MemoryResourceManager>());
  ASSERT_TRUE(runner.RunOnce().ok());

  const auto outputs = runner.Outputs("out");
  ASSERT_EQ(outputs.size(), 1U);
  EXPECT_EQ(outputs.front().Get<int>(), 23);
  EXPECT_EQ(outputs.front().timestamp(), Timestamp::FromMicroseconds(10));
  EXPECT_EQ(runner.metrics().Counter("inspect.processed"), 1U);
  EXPECT_EQ(runner.state(), NodeState::kClosed);
}

TEST(NodeTestRunnerTest, CapturesAsyncSourcePacketsAndTimestampBounds) {
  NodeTestRunner runner(std::make_unique<AsyncSourceNode>());
  ASSERT_TRUE(runner.Start().ok());
  ASSERT_TRUE(runner.WaitForOutput("out", 3, 1000).ok());
  const auto outputs = runner.TakeOutputs("out");
  ASSERT_EQ(outputs.size(), 3U);
  EXPECT_EQ(outputs[0].Get<int>(), 7);
  EXPECT_EQ(outputs[1].event(), ControlEvent::kTimestampBound);
  EXPECT_EQ(outputs[1].timestamp(), Timestamp::FromMicroseconds(8));
  EXPECT_EQ(outputs[2].event(), ControlEvent::kEndOfStream);
  EXPECT_TRUE(runner.Close().ok());
}

TEST(NodeTestRunnerTest, RestartsAStoppedNode) {
  NodeTestRunner runner(std::make_unique<AsyncSourceNode>());
  ASSERT_TRUE(runner.Start().ok());
  ASSERT_TRUE(runner.WaitForOutput("out", 3, 1000).ok());
  ASSERT_TRUE(runner.Stop().ok());
  ASSERT_TRUE(runner.Start().ok());
  ASSERT_TRUE(runner.WaitForOutput("out", 6, 1000).ok());
  EXPECT_EQ(runner.Outputs("out").size(), 6U);
  EXPECT_TRUE(runner.Close().ok());
}

TEST(NodeTestRunnerTest, CancelWakesOutputWaiter) {
  NodeTestRunner runner(std::make_unique<AsyncSourceNode>());
  ASSERT_TRUE(runner.Start().ok());
  auto result =
      std::async(std::launch::async, [&runner] { return runner.WaitForOutput("out", 100, 5000); });
  runner.Cancel();
  EXPECT_EQ(result.get().code(), StatusCode::kCancelled);
}

TEST(NodeTestRunnerTest, StartFailureClosesNode) {
  auto close_count = std::make_shared<int>(0);
  NodeTestRunner runner(std::make_unique<FailingNode>(close_count));
  const Status status = runner.Start();
  EXPECT_EQ(status.code(), StatusCode::kInternal);
  EXPECT_EQ(*close_count, 1);
  EXPECT_EQ(runner.state(), NodeState::kClosed);
}

TEST(NodeTestRunnerTest, ValidatesPortsAndRequiredInputs) {
  NodeTestRunner runner(std::make_unique<InspectNode>());
  EXPECT_EQ(runner.SetInput("missing", Packet::Make(1)).code(), StatusCode::kNotFound);
  ASSERT_TRUE(runner.Configure(ConfigValue::Object{{"offset", ConfigValue(0)}}).ok());
  ASSERT_TRUE(runner.Start().ok());
  EXPECT_EQ(runner.Process().code(), StatusCode::kInvalidArgument);
}

}  // namespace
}  // namespace rkavp::testing
