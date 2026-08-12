#include "rkavp/node_registry.hpp"

namespace {
class ExternalNode final : public rkavp::Node {
 public:
  rkavp::NodeContract Contract() const override { return {}; }

 protected:
  rkavp::Status OnProcess(rkavp::NodeContext&) override { return rkavp::Status::Ok(); }
};
}  // namespace

extern "C" int rkavp_plugin_init_v2(rkavp::NodeRegistry* registry) {
  if (registry == nullptr) return -1;
  return registry->Register("ExternalTestNode", [] { return std::make_unique<ExternalNode>(); })
                 .ok()
             ? 0
             : -1;
}
