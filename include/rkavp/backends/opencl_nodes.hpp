#pragma once

#include <functional>
#include <memory>

#include "rkavp/backends/opencl.hpp"
#include "rkavp/node.hpp"
#include "rkavp/node_registry.hpp"

namespace rkavp {

std::unique_ptr<Node> MakeOpenClKernelNode(std::unique_ptr<IOpenClBackend> backend);
Status RegisterOpenClNodes(NodeRegistry* registry,
                           std::function<std::unique_ptr<IOpenClBackend>()> factory);

}  // namespace rkavp
