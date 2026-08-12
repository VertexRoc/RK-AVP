#include "rkavp/backends/opencl_nodes.hpp"

extern "C" int rkavp_plugin_init_v2(rkavp::NodeRegistry* registry) {
  const rkavp::Status status =
      rkavp::RegisterOpenClNodes(registry, [] { return rkavp::CreateOpenClBackend(); });
  return status.ok() ? 0 : -1;
}
