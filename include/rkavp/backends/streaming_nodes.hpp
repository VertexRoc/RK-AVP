#pragma once

#include <memory>

#include "rkavp/backends/streaming.hpp"
#include "rkavp/node.hpp"
#include "rkavp/node_registry.hpp"

namespace rkavp {

std::unique_ptr<Node> MakeStreamingInputNode(std::unique_ptr<IStreamingInputBackend> backend);
std::unique_ptr<Node> MakeStreamingOutputNode(std::unique_ptr<IStreamingOutputBackend> backend);
Status RegisterStreamingNodes(
    NodeRegistry* registry, std::function<std::unique_ptr<IStreamingInputBackend>()> input_factory,
    std::function<std::unique_ptr<IStreamingOutputBackend>()> output_factory);

}  // namespace rkavp
