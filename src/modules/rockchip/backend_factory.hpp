#pragma once

#include <memory>

#include "rkavp/backends/rockchip_nodes.hpp"

namespace rkavp {

std::unique_ptr<IMppDecoderBackend> CreateRockchipMppDecoder();
std::unique_ptr<IMppEncoderBackend> CreateRockchipMppEncoder();
std::unique_ptr<IRgaBackend> CreateRockchipRga();
std::unique_ptr<IRknnBackend> CreateRockchipRknn();
std::unique_ptr<IV4l2Backend> CreateLinuxV4l2();

}  // namespace rkavp
