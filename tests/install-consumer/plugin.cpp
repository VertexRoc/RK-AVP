#include "rkavp/core.hpp"

namespace {

class ExternalPluginNode final : public rkavp::Node {
 public:
  rkavp::NodeContract Contract() const override { return {}; }

 protected:
  rkavp::Status OnProcess(rkavp::NodeContext&) override { return rkavp::Status::Ok(); }
};

}  // namespace

extern "C" int rkavp_plugin_init_v2(rkavp::NodeRegistry* registry) {
  if (registry == nullptr) return 1;
  return registry->Register("ExternalPluginNode",
                            [] { return std::make_unique<ExternalPluginNode>(); })
                 .ok()
             ? 0
             : 1;
}
