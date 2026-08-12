#include <gtest/gtest.h>

#include <atomic>
#include <cstdlib>
#include <memory>
#include <string>
#include <vector>

#include "rkavp/core.hpp"

namespace rkavp {
namespace {

class ContractNode final : public Node {
 public:
  NodeContract Contract() const override {
    return {{{"in", {MediaKind::kVideo, "nv12"}, true}},
            {{"out", {MediaKind::kVideo, "nv12"}, true}},
            {"resource"},
            InputPolicy::kAny,
            {}};
  }

 protected:
  Status OnProcess(NodeContext& context) override {
    const Packet* packet = context.Input("in");
    return packet == nullptr ? Status::Invalid("missing input") : context.Emit("out", *packet);
  }
};

class LifecycleNode final : public Node {
 public:
  explicit LifecycleNode(std::vector<std::string>* calls) : calls_(calls) {}
  NodeContract Contract() const override {
    return {{{"in", MediaCaps::Any(), true}}, {}, {}, InputPolicy::kAny, {}};
  }

 protected:
  Status OnConfigure(const NodeOptions&) override {
    calls_->push_back("configure");
    return Status::Ok();
  }
  Status OnOpen() override {
    calls_->push_back("open");
    return Status::Ok();
  }
  Status OnStart(NodeContext&) override {
    calls_->push_back("start");
    return Status::Ok();
  }
  Status OnProcess(NodeContext&) override {
    calls_->push_back("process");
    return Status::Ok();
  }
  Status OnStop() override {
    calls_->push_back("stop");
    return Status::Ok();
  }
  Status OnClose() override {
    calls_->push_back("close");
    return Status::Ok();
  }

 private:
  std::vector<std::string>* calls_;
};

GraphConfig ValidConfig() {
  GraphConfig config;
  config.name = "test";
  config.nodes = {{"a", "Contract", "default", {{"resource", "a.bin"}}},
                  {"b", "Contract", "default", {{"resource", "b.bin"}}}};
  config.edges = {{"a", "out", "b", "in", {2, QueuePolicy::kDropOldest}}};
  config.inputs = {{"input", "a", "in"}};
  config.outputs = {{"output", "b", "out"}};
  return config;
}

TEST(YamlGraphTest, ParsesStructuredValuesExecutorsPortsAndQueuePolicy) {
  ::setenv("RKAVP_TEST_RESOURCE", "env.bin", 1);
  const std::string yaml = R"(
version: 2
graph:
  name: yaml-test
  flow_control:
    input_queue: {capacity: 7, policy: drop_oldest}
    edge_queue: {capacity: 4, policy: drop_newest}
    observer_queue: {capacity: 3, policy: drop_newest}
    observe_timestamp_bounds: true
  executors:
    - {name: default, threads: 2, queue_capacity: 32}
  inputs:
    input: a.in
  outputs: {output: b.out}
  side_packets: {threshold: 0.25, enabled: true}
  nodes:
    - id: a
      type: Contract
      options:
        resource: "${RKAVP_TEST_RESOURCE:-fallback.bin}"
        dimensions: [1, 3, 224, 224]
    - id: b
      type: Contract
      options: {resource: fallback.bin}
  edges:
    - from: a.out
      to: b.in
)";
  GraphConfig config;
  ASSERT_TRUE(YamlGraphLoader::LoadString(yaml, &config).ok());
  EXPECT_EQ(config.executors[0].threads, 2U);
  EXPECT_EQ(config.nodes[0].options.at("resource").As<std::string>(), "env.bin");
  EXPECT_EQ(config.nodes[0].options.at("dimensions").AsArray().size(), 4U);
  EXPECT_TRUE(config.side_packets.at("enabled").As<bool>());
  EXPECT_EQ(config.edges[0].queue.policy, QueuePolicy::kDropNewest);
  EXPECT_EQ(config.inputs[0].queue.capacity, 7U);
  EXPECT_EQ(config.flow_control.observer_queue.capacity, 3U);
  EXPECT_EQ(config.flow_control.observer_queue.policy, QueuePolicy::kDropNewest);
  EXPECT_TRUE(config.flow_control.observe_timestamp_bounds);
}

TEST(YamlGraphTest, ReturnsActionableErrors) {
  GraphConfig config;
  Status status = YamlGraphLoader::LoadString("version: 3\ngraph: {nodes: []}\n", &config);
  EXPECT_EQ(status.code(), StatusCode::kInvalidArgument);
  EXPECT_NE(status.message().find("unsupported graph version"), std::string::npos);
  status =
      YamlGraphLoader::LoadString("version: 1\ngraph: {flow_control: {}, nodes: []}\n", &config);
  EXPECT_NE(status.message().find("requires version: 2"), std::string::npos);
}

TEST(GraphValidationTest, ValidatesExecutorsPortsOptionsCyclesAndBackEdges) {
  NodeRegistry registry;
  ASSERT_TRUE(registry.Register("Contract", [] { return std::make_unique<ContractNode>(); }).ok());
  GraphConfig valid = ValidConfig();
  EXPECT_TRUE(Graph(valid, &registry).Validate().ok());

  GraphConfig bad_executor = valid;
  bad_executor.nodes[0].executor = "missing";
  EXPECT_EQ(Graph(bad_executor, &registry).Validate().code(), StatusCode::kNotFound);

  GraphConfig cycle = valid;
  cycle.edges.push_back({"b", "out", "a", "in", {2, QueuePolicy::kDropOldest}});
  EXPECT_EQ(Graph(cycle, &registry).Validate().code(), StatusCode::kInvalidArgument);

  cycle.edges.back().back_edge = true;
  EXPECT_EQ(Graph(cycle, &registry).Validate().code(), StatusCode::kInvalidArgument);
  cycle.edges.back().initial_packet = ConfigValue(0);
  EXPECT_TRUE(Graph(cycle, &registry).Validate().ok());

  GraphConfig blocking_observer = valid;
  blocking_observer.flow_control.observer_queue.policy = QueuePolicy::kBlock;
  const Status observer_status = Graph(blocking_observer, &registry).Validate();
  EXPECT_EQ(observer_status.code(), StatusCode::kInvalidArgument);
  EXPECT_NE(observer_status.message().find("cannot be block"), std::string::npos);
}

TEST(LifecycleTest, EnforcesConfigureOpenStartStopCloseOrder) {
  std::vector<std::string> calls;
  LifecycleNode node(&calls);
  std::atomic<bool> cancelled{false};
  MetricsRegistry metrics;
  NodeContext context(
      "node", "default", {{"in", Packet::Make(1)}},
      [](const std::string&, Packet) { return Status::Ok(); }, nullptr, nullptr, nullptr, &metrics,
      &cancelled);
  EXPECT_EQ(node.Start(context).code(), StatusCode::kFailedPrecondition);
  ASSERT_TRUE(node.Configure({}).ok());
  ASSERT_TRUE(node.Open().ok());
  ASSERT_TRUE(node.Start(context).ok());
  ASSERT_TRUE(node.Process(context).ok());
  ASSERT_TRUE(node.Stop().ok());
  ASSERT_TRUE(node.Close().ok());
  EXPECT_EQ(calls,
            (std::vector<std::string>{"configure", "open", "start", "process", "stop", "close"}));
}

TEST(SubgraphTest, PrefixesEndpointsAndAppliesOverrides) {
  GraphConfig parent;
  GraphConfig child = ValidConfig();
  ASSERT_TRUE(MergeSubgraph(&parent, child, "media", {{"a.resource", "override.bin"}}).ok());
  ASSERT_EQ(parent.nodes.size(), 2U);
  EXPECT_EQ(parent.nodes[0].id, "media.a");
  EXPECT_EQ(parent.nodes[0].options.at("resource").As<std::string>(), "override.bin");
  EXPECT_EQ(parent.edges[0].to_node, "media.b");
}

TEST(ServicesTest, SidePacketsAreImmutableAndServicesAreTyped) {
  SidePacketSet packets;
  ASSERT_TRUE(packets.Set("value", Packet::Make(7)).ok());
  EXPECT_EQ(packets.Set("value", Packet::Make(8)).code(), StatusCode::kAlreadyExists);
  ASSERT_NE(packets.Find("value"), nullptr);
  EXPECT_EQ(packets.Find("value")->Get<int>(), 7);
  GraphServiceRegistry services;
  auto value = std::make_shared<int>(42);
  ASSERT_TRUE(services.Set("answer", value).ok());
  EXPECT_EQ(*services.Get<int>("answer"), 42);
  EXPECT_EQ(services.Get<std::string>("answer"), nullptr);
}

TEST(PluginRegistryTest, RegistersCreatesAndUnloadsFactories) {
  NodeRegistry registry;
  ASSERT_TRUE(registry.Register("Contract", [] { return std::make_unique<ContractNode>(); }).ok());
  EXPECT_NE(registry.Create("Contract"), nullptr);
  ASSERT_TRUE(registry.Unregister("Contract").ok());
  EXPECT_FALSE(registry.Contains("Contract"));
}

TEST(PluginRegistryTest, LoadsAndUnloadsVersionedSharedLibrary) {
  NodeRegistry registry;
  PluginManager manager(&registry);
  ASSERT_TRUE(manager.Load(RKAVP_TEST_PLUGIN_PATH).ok());
  EXPECT_TRUE(registry.Contains("ExternalTestNode"));
  std::unique_ptr<Node> node = registry.Create("ExternalTestNode");
  ASSERT_NE(node, nullptr);
  ASSERT_TRUE(manager.Unload(RKAVP_TEST_PLUGIN_PATH).ok());
  EXPECT_FALSE(registry.Contains("ExternalTestNode"));
  EXPECT_TRUE(node->Configure({}).ok());
  node.reset();
}

TEST(PluginRegistryTest, RejectsLegacyV1PluginApi) {
  NodeRegistry registry;
  PluginManager manager(&registry);
  const Status status = manager.Load(RKAVP_TEST_PLUGIN_V1_PATH);
  EXPECT_EQ(status.code(), StatusCode::kInvalidArgument);
  EXPECT_NE(status.message().find("rkavp_plugin_init_v2"), std::string::npos);
}

}  // namespace
}  // namespace rkavp
