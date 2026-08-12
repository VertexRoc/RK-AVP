#include "rkavp/backends/streaming_nodes.hpp"

namespace rkavp {
std::unique_ptr<IStreamingInputBackend> CreateStreamingInputBackend();
std::unique_ptr<IStreamingOutputBackend> CreateStreamingOutputBackend();
}  // namespace rkavp

extern "C" int rkavp_plugin_init_v2(rkavp::NodeRegistry* registry) {
  const rkavp::Status status = rkavp::RegisterStreamingNodes(
      registry, [] { return rkavp::CreateStreamingInputBackend(); },
      [] { return rkavp::CreateStreamingOutputBackend(); });
  return status.ok() ? 0 : -1;
}
