#include <rknn_api.h>

#include <algorithm>
#include <cstring>
#include <fstream>
#include <limits>
#include <mutex>
#include <utility>
#include <vector>

#include "../backend_factory.hpp"

namespace rkavp {
namespace {

Status RknnStatus(int result, const char* operation) {
  return result == RKNN_SUCC
             ? Status::Ok()
             : Status::Unavailable(std::string(operation) + " failed: " + std::to_string(result));
}

TensorDataType TensorType(rknn_tensor_type type) {
  switch (type) {
    case RKNN_TENSOR_FLOAT32:
      return TensorDataType::kFloat32;
    case RKNN_TENSOR_FLOAT16:
      return TensorDataType::kFloat16;
    case RKNN_TENSOR_INT64:
      return TensorDataType::kInt64;
    case RKNN_TENSOR_INT32:
      return TensorDataType::kInt32;
    case RKNN_TENSOR_INT16:
      return TensorDataType::kInt16;
    case RKNN_TENSOR_INT8:
      return TensorDataType::kInt8;
    case RKNN_TENSOR_UINT8:
      return TensorDataType::kUInt8;
    case RKNN_TENSOR_BOOL:
      return TensorDataType::kBool;
    default:
      return TensorDataType::kUnknown;
  }
}

TensorLayout TensorFormat(rknn_tensor_format format) {
  switch (format) {
    case RKNN_TENSOR_NCHW:
      return TensorLayout::kNchw;
    case RKNN_TENSOR_NHWC:
      return TensorLayout::kNhwc;
    case RKNN_TENSOR_NC1HWC2:
      return TensorLayout::kNc1hwc2;
    default:
      return TensorLayout::kAny;
  }
}

TensorDesc Describe(const rknn_tensor_attr& attr) {
  TensorDesc desc;
  desc.name = attr.name;
  desc.shape.reserve(attr.n_dims);
  for (std::uint32_t i = 0; i < attr.n_dims; ++i) desc.shape.push_back(attr.dims[i]);
  desc.data_type = TensorType(attr.type);
  desc.layout = TensorFormat(attr.fmt);
  desc.byte_size = attr.size;
  desc.byte_size_with_stride = attr.size_with_stride != 0 ? attr.size_with_stride : attr.size;
  desc.width_stride = attr.w_stride;
  if (attr.qnt_type == RKNN_TENSOR_QNT_AFFINE_ASYMMETRIC) {
    desc.quantization.type = QuantizationType::kAffineAsymmetric;
    desc.quantization.zero_point = attr.zp;
    desc.quantization.scale = attr.scale;
  } else if (attr.qnt_type == RKNN_TENSOR_QNT_DFP) {
    desc.quantization.type = QuantizationType::kDynamicFixedPoint;
    desc.quantization.fractional_length = attr.fl;
  }
  return desc;
}

rknn_core_mask CoreMask(RknnCoreMask mask) {
  switch (mask) {
    case RknnCoreMask::kCore0:
      return RKNN_NPU_CORE_0;
    case RknnCoreMask::kCore1:
      return RKNN_NPU_CORE_1;
    case RknnCoreMask::kCore2:
      return RKNN_NPU_CORE_2;
    case RknnCoreMask::kCore01:
      return RKNN_NPU_CORE_0_1;
    case RknnCoreMask::kCore012:
      return RKNN_NPU_CORE_0_1_2;
    case RknnCoreMask::kAuto:
      return RKNN_NPU_CORE_AUTO;
  }
  return RKNN_NPU_CORE_AUTO;
}

class RockchipRknnBackend final : public IRknnBackend {
 public:
  ~RockchipRknnBackend() override { Close(); }

  Status LoadModel(const RknnModelConfig& config) override {
    std::lock_guard<std::mutex> lock(mutex_);
    CloseLocked();
    if (config.path.empty()) return Status::Invalid("RKNN model path is empty");
    if (config.zero_copy) {
      return Status::Invalid(
          "RKNN zero_copy=true is not implemented by this backend; use cached tensor memory or "
          "provide an external backend plugin");
    }
    if (!config.cache_tensor_memory) {
      return Status::Invalid("RKNN cache_tensor_memory=false is not implemented by this backend");
    }

    std::ifstream stream(config.path, std::ios::binary | std::ios::ate);
    if (!stream) return Status::NotFound("cannot open RKNN model: " + config.path);
    const std::streamsize length = stream.tellg();
    if (length <= 0 ||
        static_cast<std::uint64_t>(length) > std::numeric_limits<std::uint32_t>::max()) {
      return Status::Invalid("RKNN model has invalid size: " + config.path);
    }
    model_.resize(static_cast<std::size_t>(length));
    stream.seekg(0, std::ios::beg);
    if (!stream.read(reinterpret_cast<char*>(model_.data()), length)) {
      model_.clear();
      return Status::Unavailable("cannot read RKNN model: " + config.path);
    }

    Status status =
        RknnStatus(rknn_init(&context_, model_.data(), static_cast<std::uint32_t>(model_.size()),
                             RKNN_FLAG_COLLECT_PERF_MASK, nullptr),
                   "rknn_init");
    if (!status.ok()) {
      CloseLocked();
      return status;
    }
    status =
        RknnStatus(rknn_set_core_mask(context_, CoreMask(config.core_mask)), "rknn_set_core_mask");
    if (!status.ok()) {
      CloseLocked();
      return status;
    }
    config_ = config;
    status = RefreshAttributesLocked();
    if (status.ok()) status = AllocateIoMemoryLocked();
    if (!status.ok()) CloseLocked();
    return status;
  }

  Status QueryInputs(std::vector<TensorDesc>* inputs) const override {
    if (inputs == nullptr) return Status::Invalid("input descriptor output is null");
    std::lock_guard<std::mutex> lock(mutex_);
    if (context_ == 0) return Status::FailedPrecondition("RKNN model is not loaded");
    *inputs = input_descs_;
    return Status::Ok();
  }

  Status QueryOutputs(std::vector<TensorDesc>* outputs) const override {
    if (outputs == nullptr) return Status::Invalid("output descriptor output is null");
    std::lock_guard<std::mutex> lock(mutex_);
    if (context_ == 0) return Status::FailedPrecondition("RKNN model is not loaded");
    *outputs = output_descs_;
    return Status::Ok();
  }

  Status SetDynamicShape(const std::vector<std::vector<std::int64_t>>& shapes) override {
    std::lock_guard<std::mutex> lock(mutex_);
    if (context_ == 0) return Status::FailedPrecondition("RKNN model is not loaded");
    if (shapes.size() != input_attrs_.size())
      return Status::Invalid("dynamic shape input count mismatch");
    std::vector<rknn_tensor_attr> attrs = input_attrs_;
    for (std::size_t i = 0; i < shapes.size(); ++i) {
      if (shapes[i].empty() || shapes[i].size() > RKNN_MAX_DIMS)
        return Status::Invalid("invalid RKNN dynamic shape rank");
      attrs[i].n_dims = static_cast<std::uint32_t>(shapes[i].size());
      for (std::size_t dimension = 0; dimension < shapes[i].size(); ++dimension) {
        const std::int64_t value = shapes[i][dimension];
        if (value <= 0 || value > std::numeric_limits<std::uint32_t>::max()) {
          return Status::Invalid("invalid RKNN dynamic shape dimension");
        }
        attrs[i].dims[dimension] = static_cast<std::uint32_t>(value);
      }
    }
    Status status = RknnStatus(
        rknn_set_input_shapes(context_, static_cast<std::uint32_t>(attrs.size()), attrs.data()),
        "rknn_set_input_shapes");
    if (status.ok()) status = RefreshAttributesLocked(true);
    if (status.ok()) status = AllocateIoMemoryLocked();
    return status;
  }

  Status Infer(const TensorSet& inputs, TensorSet* outputs) override {
    if (outputs == nullptr) return Status::Invalid("RKNN output TensorSet is null");
    std::lock_guard<std::mutex> lock(mutex_);
    if (context_ == 0) return Status::FailedPrecondition("RKNN model is not loaded");
    if (inputs.size() != input_memory_.size())
      return Status::Invalid("RKNN input tensor count mismatch");

    for (std::size_t i = 0; i < inputs.size(); ++i) {
      if (!inputs[i].buffer) return Status::Invalid("RKNN input tensor has no buffer");
      const std::size_t required = input_attrs_[i].size;
      if (inputs[i].buffer->size() < required || input_memory_[i]->size < required) {
        return Status::Invalid("RKNN input tensor is smaller than the model input");
      }
      if (inputs[i].buffer->fence()) {
        Status status = inputs[i].buffer->fence()->Wait(-1);
        if (!status.ok()) return status;
      }
      Status status = inputs[i].buffer->BeginCpuAccess(MapAccess::kRead);
      if (!status.ok()) return status;
      void* source = inputs[i].buffer->Map(MapAccess::kRead);
      if (source == nullptr || input_memory_[i]->virt_addr == nullptr) {
        inputs[i].buffer->Unmap();
        inputs[i].buffer->EndCpuAccess(MapAccess::kRead);
        return Status::Invalid("RKNN input tensor is not CPU mappable");
      }
      std::memcpy(input_memory_[i]->virt_addr, source, required);
      inputs[i].buffer->Unmap();
      status = inputs[i].buffer->EndCpuAccess(MapAccess::kRead);
      if (!status.ok()) return status;
      status = RknnStatus(rknn_mem_sync(context_, input_memory_[i], RKNN_MEMORY_SYNC_TO_DEVICE),
                          "rknn_mem_sync input");
      if (!status.ok()) return status;
    }

    Status status = RknnStatus(rknn_run(context_, nullptr), "rknn_run");
    if (!status.ok()) return status;

    TensorSet result;
    result.reserve(output_memory_.size());
    for (std::size_t i = 0; i < output_memory_.size(); ++i) {
      status = RknnStatus(rknn_mem_sync(context_, output_memory_[i], RKNN_MEMORY_SYNC_FROM_DEVICE),
                          "rknn_mem_sync output");
      if (!status.ok()) return status;
      const std::size_t size = output_descs_[i].byte_size_with_stride;
      if (output_memory_[i]->virt_addr == nullptr || output_memory_[i]->size < size) {
        return Status::Internal("RKNN output memory is invalid");
      }
      std::vector<std::uint8_t> bytes(size);
      std::memcpy(bytes.data(), output_memory_[i]->virt_addr, size);
      result.push_back({output_descs_[i], std::make_shared<HostBuffer>(std::move(bytes))});
    }
    *outputs = std::move(result);
    return Status::Ok();
  }

  Status QueryPerformance(RknnPerformance* performance) const override {
    if (performance == nullptr) return Status::Invalid("RKNN performance output is null");
    std::lock_guard<std::mutex> lock(mutex_);
    if (context_ == 0) return Status::FailedPrecondition("RKNN model is not loaded");
    rknn_perf_run perf{};
    Status status = RknnStatus(rknn_query(context_, RKNN_QUERY_PERF_RUN, &perf, sizeof(perf)),
                               "RKNN_QUERY_PERF_RUN");
    if (!status.ok()) return status;
    performance->run_duration_us = perf.run_duration;
    performance->sdk_version = sdk_version_;
    performance->driver_version = driver_version_;
    return Status::Ok();
  }

  void Close() override {
    std::lock_guard<std::mutex> lock(mutex_);
    CloseLocked();
  }

 private:
  Status QueryAttrsLocked(rknn_query_cmd command, std::uint32_t count,
                          std::vector<rknn_tensor_attr>* attrs) {
    attrs->assign(count, {});
    for (std::uint32_t i = 0; i < count; ++i) {
      (*attrs)[i].index = i;
      Status status = RknnStatus(rknn_query(context_, command, &(*attrs)[i], sizeof((*attrs)[i])),
                                 "rknn_query tensor attribute");
      if (!status.ok()) return status;
    }
    return Status::Ok();
  }

  Status RefreshAttributesLocked(bool current = false) {
    rknn_input_output_num count{};
    Status status = RknnStatus(rknn_query(context_, RKNN_QUERY_IN_OUT_NUM, &count, sizeof(count)),
                               "RKNN_QUERY_IN_OUT_NUM");
    if (!status.ok()) return status;
    const rknn_query_cmd input_command =
        current ? RKNN_QUERY_CURRENT_INPUT_ATTR : RKNN_QUERY_INPUT_ATTR;
    const rknn_query_cmd output_command =
        current ? RKNN_QUERY_CURRENT_OUTPUT_ATTR : RKNN_QUERY_OUTPUT_ATTR;
    const rknn_query_cmd native_input_command =
        current ? RKNN_QUERY_CURRENT_NATIVE_INPUT_ATTR : RKNN_QUERY_NATIVE_INPUT_ATTR;
    const rknn_query_cmd native_output_command =
        current ? RKNN_QUERY_CURRENT_NATIVE_OUTPUT_ATTR : RKNN_QUERY_NATIVE_OUTPUT_ATTR;
    status = QueryAttrsLocked(input_command, count.n_input, &input_attrs_);
    if (status.ok()) status = QueryAttrsLocked(output_command, count.n_output, &output_attrs_);
    if (status.ok())
      status = QueryAttrsLocked(native_input_command, count.n_input, &native_input_attrs_);
    if (status.ok())
      status = QueryAttrsLocked(native_output_command, count.n_output, &native_output_attrs_);
    if (!status.ok()) return status;

    input_descs_.clear();
    output_descs_.clear();
    for (const auto& attr : input_attrs_) input_descs_.push_back(Describe(attr));
    for (const auto& attr : output_attrs_) output_descs_.push_back(Describe(attr));
    rknn_sdk_version version{};
    status = RknnStatus(rknn_query(context_, RKNN_QUERY_SDK_VERSION, &version, sizeof(version)),
                        "RKNN_QUERY_SDK_VERSION");
    if (status.ok()) {
      sdk_version_ = version.api_version;
      driver_version_ = version.drv_version;
    }
    return status;
  }

  void DestroyIoMemoryLocked() {
    for (rknn_tensor_mem* memory : input_memory_)
      if (memory != nullptr) rknn_destroy_mem(context_, memory);
    for (rknn_tensor_mem* memory : output_memory_)
      if (memory != nullptr) rknn_destroy_mem(context_, memory);
    input_memory_.clear();
    output_memory_.clear();
  }

  Status AllocateIoMemoryLocked() {
    DestroyIoMemoryLocked();
    input_memory_.reserve(native_input_attrs_.size());
    output_memory_.reserve(native_output_attrs_.size());
    for (auto& attr : native_input_attrs_) {
      const std::uint32_t size = attr.size_with_stride != 0 ? attr.size_with_stride : attr.size;
      rknn_tensor_mem* memory = rknn_create_mem(context_, size);
      if (memory == nullptr) return Status::Unavailable("rknn_create_mem input failed");
      input_memory_.push_back(memory);
      Status status = RknnStatus(rknn_set_io_mem(context_, memory, &attr), "rknn_set_io_mem input");
      if (!status.ok()) return status;
    }
    for (auto& attr : native_output_attrs_) {
      const std::uint32_t size = attr.size_with_stride != 0 ? attr.size_with_stride : attr.size;
      rknn_tensor_mem* memory = rknn_create_mem(context_, size);
      if (memory == nullptr) return Status::Unavailable("rknn_create_mem output failed");
      output_memory_.push_back(memory);
      Status status =
          RknnStatus(rknn_set_io_mem(context_, memory, &attr), "rknn_set_io_mem output");
      if (!status.ok()) return status;
    }
    return Status::Ok();
  }

  void CloseLocked() {
    if (context_ != 0) DestroyIoMemoryLocked();
    if (context_ != 0) rknn_destroy(context_);
    context_ = 0;
    model_.clear();
    input_attrs_.clear();
    output_attrs_.clear();
    native_input_attrs_.clear();
    native_output_attrs_.clear();
    input_descs_.clear();
    output_descs_.clear();
    sdk_version_.clear();
    driver_version_.clear();
  }

  mutable std::mutex mutex_;
  rknn_context context_ = 0;
  RknnModelConfig config_;
  std::vector<std::uint8_t> model_;
  std::vector<rknn_tensor_attr> input_attrs_;
  std::vector<rknn_tensor_attr> output_attrs_;
  std::vector<rknn_tensor_attr> native_input_attrs_;
  std::vector<rknn_tensor_attr> native_output_attrs_;
  std::vector<TensorDesc> input_descs_;
  std::vector<TensorDesc> output_descs_;
  std::vector<rknn_tensor_mem*> input_memory_;
  std::vector<rknn_tensor_mem*> output_memory_;
  std::string sdk_version_;
  std::string driver_version_;
};

}  // namespace

std::unique_ptr<IRknnBackend> CreateRockchipRknn() {
  return std::make_unique<RockchipRknnBackend>();
}

}  // namespace rkavp
