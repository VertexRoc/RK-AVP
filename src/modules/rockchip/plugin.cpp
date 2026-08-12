#include <memory>

#include "backend_factory.hpp"

namespace rkavp {
namespace {

class DefaultRockchipBackendFactory final : public RockchipBackendFactory {
 public:
  std::unique_ptr<IMppDecoderBackend> CreateMppDecoder() override {
    return CreateRockchipMppDecoder();
  }
  std::unique_ptr<IMppEncoderBackend> CreateMppEncoder() override {
    return CreateRockchipMppEncoder();
  }
  std::unique_ptr<IRgaBackend> CreateRga() override { return CreateRockchipRga(); }
  std::unique_ptr<IRknnBackend> CreateRknn() override { return CreateRockchipRknn(); }
  std::unique_ptr<IV4l2Backend> CreateV4l2() override { return CreateLinuxV4l2(); }
};

}  // namespace
}  // namespace rkavp

extern "C" int rkavp_plugin_init_v2(rkavp::NodeRegistry* registry) {
  const rkavp::Status status = rkavp::RegisterRockchipNodes(
      registry, std::make_shared<rkavp::DefaultRockchipBackendFactory>());
  return status.ok() ? 0 : -1;
}
