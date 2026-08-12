#include "rkavp/core.hpp"
#include "rkavp/testing/node_test_runner.hpp"

namespace {

class ExternalNode final : public rkavp::Node {
 public:
  rkavp::NodeContract Contract() const override {
    return {{{"in", rkavp::MediaCaps::Any(), true}}, {{"out", rkavp::MediaCaps::Any(), true}}, {}};
  }

 protected:
  rkavp::Status OnProcess(rkavp::NodeContext& context) override {
    const rkavp::Packet* input = context.Input("in");
    return input == nullptr ? rkavp::Status::Invalid("missing input") : context.Emit("out", *input);
  }
};

}  // namespace

int main() {
  rkavp::testing::NodeTestRunner runner(std::make_unique<ExternalNode>());
  if (!runner.SetInput("in", rkavp::Packet::Make(42)).ok() || !runner.RunOnce().ok()) return 1;
  const auto outputs = runner.Outputs("out");
  if (outputs.size() != 1 || outputs.front().Get<int>() != 42) return 2;

  rkavp::NodeRegistry registry;
  rkavp::RegisterBuiltinNodes(&registry);
  rkavp::PluginManager plugins(&registry);
  if (!plugins.Load(RKAVP_EXTERNAL_PLUGIN_PATH).ok()) return 3;
  if (!registry.Contains("ExternalPluginNode")) return 4;
  auto plugin_node = registry.Create("ExternalPluginNode");
  if (!plugin_node) return 5;
  plugin_node.reset();
  if (!plugins.Unload(RKAVP_EXTERNAL_PLUGIN_PATH).ok()) return 6;
  if (registry.Contains("ExternalPluginNode")) return 7;

  const rkavp::Status incompatible = plugins.Load(RKAVP_EXTERNAL_PLUGIN_V1_PATH);
  if (incompatible.ok() || incompatible.code() != rkavp::StatusCode::kInvalidArgument) return 8;
  return 0;
}
