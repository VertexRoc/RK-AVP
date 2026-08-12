#include "rkavp/backends/audio_nodes.hpp"

namespace rkavp {
std::unique_ptr<IAlsaBackend> CreateAlsaBackend();
}

extern "C" int rkavp_plugin_init_v2(rkavp::NodeRegistry* registry) {
  const rkavp::Status status =
      rkavp::RegisterAudioNodes(registry, [] { return rkavp::CreateAlsaBackend(); });
  return status.ok() ? 0 : -1;
}
