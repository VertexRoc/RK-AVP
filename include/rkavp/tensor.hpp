#pragma once

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "rkavp/buffer.hpp"

namespace rkavp {

enum class TensorDataType {
  kUnknown,
  kFloat32,
  kFloat16,
  kInt64,
  kInt32,
  kInt16,
  kInt8,
  kUInt8,
  kBool
};
enum class TensorLayout { kAny, kNchw, kNhwc, kNc1hwc2 };
enum class QuantizationType { kNone, kAffineAsymmetric, kDynamicFixedPoint };

struct QuantizationParams {
  QuantizationType type = QuantizationType::kNone;
  std::int32_t zero_point = 0;
  float scale = 1.0F;
  std::int32_t fractional_length = 0;
};

struct TensorDesc {
  std::string name;
  std::vector<std::int64_t> shape;
  TensorDataType data_type = TensorDataType::kUnknown;
  TensorLayout layout = TensorLayout::kAny;
  QuantizationParams quantization;
  std::size_t byte_size = 0;
  std::size_t byte_size_with_stride = 0;
  std::size_t width_stride = 0;
  bool dynamic = false;
};

struct Tensor {
  TensorDesc desc;
  BufferPtr buffer;
};

using TensorSet = std::vector<Tensor>;

}  // namespace rkavp
