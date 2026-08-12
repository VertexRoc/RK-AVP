#pragma once

#include <memory>

#include "rkavp/backends/alsa.hpp"
#include "rkavp/node.hpp"
#include "rkavp/node_registry.hpp"

namespace rkavp {

std::unique_ptr<Node> MakeAlsaCaptureNode(std::unique_ptr<IAlsaBackend> backend);
Status RegisterAudioNodes(NodeRegistry* registry,
                          std::function<std::unique_ptr<IAlsaBackend>()> factory);

}  // namespace rkavp
