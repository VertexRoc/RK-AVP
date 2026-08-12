#pragma once

#include <memory>
#include <string>
#include <vector>

#include "rkavp/buffer.hpp"
#include "rkavp/status.hpp"

namespace rkavp {

struct OpenClKernelConfig {
  std::string name;
  std::string source;
  std::string build_options;
};

class IOpenClBackend {
 public:
  virtual ~IOpenClBackend() = default;
  virtual Status Compile(const OpenClKernelConfig& config) = 0;
  virtual Status Run(const std::string& kernel, const std::vector<BufferPtr>& inputs,
                     const std::vector<BufferPtr>& outputs,
                     const std::vector<std::size_t>& global_work_size,
                     std::shared_ptr<Fence>* fence) = 0;
  virtual void Close() = 0;
};

std::unique_ptr<IOpenClBackend> CreateOpenClBackend();

}  // namespace rkavp
