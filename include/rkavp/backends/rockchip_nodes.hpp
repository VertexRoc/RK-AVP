#pragma once

#include <memory>

#include "rkavp/backends/mpp.hpp"
#include "rkavp/backends/rga.hpp"
#include "rkavp/backends/rknn.hpp"
#include "rkavp/backends/v4l2.hpp"
#include "rkavp/node.hpp"
#include "rkavp/node_registry.hpp"

namespace rkavp {

class RockchipBackendFactory {
 public:
  virtual ~RockchipBackendFactory() = default;
  virtual std::unique_ptr<IMppDecoderBackend> CreateMppDecoder() = 0;
  virtual std::unique_ptr<IMppEncoderBackend> CreateMppEncoder() = 0;
  virtual std::unique_ptr<IRgaBackend> CreateRga() = 0;
  virtual std::unique_ptr<IRknnBackend> CreateRknn() = 0;
  virtual std::unique_ptr<IV4l2Backend> CreateV4l2() = 0;
};

std::unique_ptr<Node> MakeMppDecoderNode(std::unique_ptr<IMppDecoderBackend> backend);
std::unique_ptr<Node> MakeMppEncoderNode(std::unique_ptr<IMppEncoderBackend> backend);
std::unique_ptr<Node> MakeRgaTransformNode(std::unique_ptr<IRgaBackend> backend);
std::unique_ptr<Node> MakeRknnInferenceNode(std::unique_ptr<IRknnBackend> backend);
std::unique_ptr<Node> MakeRknnBatchInferenceNode(std::unique_ptr<IRknnBackend> backend);
std::unique_ptr<Node> MakeV4l2SourceNode(std::unique_ptr<IV4l2Backend> backend);
Status RegisterRockchipNodes(NodeRegistry* registry,
                             std::shared_ptr<RockchipBackendFactory> factory);

}  // namespace rkavp
