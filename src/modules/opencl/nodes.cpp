#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "rkavp/backends/opencl_nodes.hpp"
#include "rkavp/tensor.hpp"

namespace rkavp {
namespace {

Status PositiveSizeArray(const NodeOptions& options, const std::string& name,
                         std::vector<std::size_t>* values) {
  const ConfigValue* option = FindOption(options, name);
  if (option == nullptr || !option->Is<std::shared_ptr<ConfigValue::Array>>()) {
    return Status::Invalid(name + " must be a list of positive integers");
  }
  values->clear();
  for (const ConfigValue& item : option->AsArray()) {
    if (!item.Is<std::int64_t>() || item.As<std::int64_t>() <= 0) {
      return Status::Invalid(name + " must contain positive integers");
    }
    values->push_back(static_cast<std::size_t>(item.As<std::int64_t>()));
  }
  return values->empty() ? Status::Invalid(name + " cannot be empty") : Status::Ok();
}

class OpenClKernelNode final : public Node {
 public:
  explicit OpenClKernelNode(std::unique_ptr<IOpenClBackend> backend)
      : backend_(std::move(backend)) {}
  ~OpenClKernelNode() override { backend_->Close(); }

  NodeContract Contract() const override {
    return {{{"tensors", {MediaKind::kTensor}, true}},
            {{"tensors", {MediaKind::kTensor}, true}},
            {"kernel", "source", "global_work_size", "output_sizes"},
            InputPolicy::kAny,
            {}};
  }

 protected:
  Status OnConfigure(const NodeOptions& options) override {
    Status status = GetStringOption(options, "kernel", &kernel_.name);
    if (status.ok()) status = GetStringOption(options, "source", &kernel_.source);
    if (const ConfigValue* build_options = FindOption(options, "build_options")) {
      if (!build_options->Is<std::string>())
        return Status::Invalid("build_options must be a string");
      kernel_.build_options = build_options->As<std::string>();
    }
    if (status.ok()) status = PositiveSizeArray(options, "global_work_size", &global_work_size_);
    if (status.ok()) status = PositiveSizeArray(options, "output_sizes", &output_sizes_);
    return status;
  }

  Status OnOpen() override { return backend_->Compile(kernel_); }

  Status OnProcess(NodeContext& context) override {
    const Packet* packet = context.Input("tensors");
    if (packet == nullptr || !packet->Is<TensorSet>()) {
      return Status::Invalid("OpenClKernel expects TensorSet input");
    }
    std::vector<BufferPtr> inputs;
    for (const Tensor& tensor : packet->Get<TensorSet>()) {
      if (!tensor.buffer) return Status::Invalid("OpenClKernel input tensor has no buffer");
      inputs.push_back(tensor.buffer);
    }
    TensorSet outputs;
    std::vector<BufferPtr> output_buffers;
    for (std::size_t index = 0; index < output_sizes_.size(); ++index) {
      BufferPtr buffer = std::make_shared<HostBuffer>(output_sizes_[index]);
      TensorDesc desc;
      desc.name = "output_" + std::to_string(index);
      desc.shape = {static_cast<std::int64_t>(output_sizes_[index])};
      desc.data_type = TensorDataType::kUInt8;
      desc.byte_size = output_sizes_[index];
      desc.byte_size_with_stride = output_sizes_[index];
      output_buffers.push_back(buffer);
      outputs.push_back({std::move(desc), std::move(buffer)});
    }
    std::shared_ptr<Fence> fence;
    Status status = backend_->Run(kernel_.name, inputs, output_buffers, global_work_size_, &fence);
    if (!status.ok()) return status;
    for (Tensor& tensor : outputs) tensor.buffer->set_fence(fence);
    return context.Emit("tensors", Packet::Make(std::move(outputs), packet->timestamp()));
  }

  Status OnClose() override {
    backend_->Close();
    return Status::Ok();
  }

 private:
  std::unique_ptr<IOpenClBackend> backend_;
  OpenClKernelConfig kernel_;
  std::vector<std::size_t> global_work_size_;
  std::vector<std::size_t> output_sizes_;
};

}  // namespace

std::unique_ptr<Node> MakeOpenClKernelNode(std::unique_ptr<IOpenClBackend> backend) {
  return std::make_unique<OpenClKernelNode>(std::move(backend));
}

Status RegisterOpenClNodes(NodeRegistry* registry,
                           std::function<std::unique_ptr<IOpenClBackend>()> factory) {
  if (registry == nullptr || !factory) return Status::Invalid("OpenCL backend factory is required");
  return registry->Register("OpenClKernel", [factory] { return MakeOpenClKernelNode(factory()); });
}

}  // namespace rkavp
