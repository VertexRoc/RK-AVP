#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "rkavp/status.hpp"
#include "rkavp/tensor.hpp"

namespace rkavp {

enum class RknnCoreMask { kAuto, kCore0, kCore1, kCore2, kCore01, kCore012 };

struct RknnModelConfig {
  std::string path;
  RknnCoreMask core_mask = RknnCoreMask::kAuto;
  bool zero_copy = false;
  bool cache_tensor_memory = true;
};

struct RknnPerformance {
  std::int64_t run_duration_us = 0;
  std::string sdk_version;
  std::string driver_version;
};

class IRknnBackend {
 public:
  virtual ~IRknnBackend() = default;
  virtual Status LoadModel(const RknnModelConfig& config) = 0;
  virtual Status QueryInputs(std::vector<TensorDesc>* inputs) const = 0;
  virtual Status QueryOutputs(std::vector<TensorDesc>* outputs) const = 0;
  virtual Status SetDynamicShape(const std::vector<std::vector<std::int64_t>>& shapes) = 0;
  virtual Status Infer(const TensorSet& inputs, TensorSet* outputs) = 0;
  virtual Status QueryPerformance(RknnPerformance* performance) const = 0;
  virtual void Close() = 0;
};

}  // namespace rkavp
