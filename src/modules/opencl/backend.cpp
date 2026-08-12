#define CL_TARGET_OPENCL_VERSION 120
#include <CL/cl.h>

#include <chrono>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

#include "rkavp/backends/opencl.hpp"

namespace rkavp {
namespace {

Status OpenClStatus(cl_int result, const char* operation) {
  return result == CL_SUCCESS
             ? Status::Ok()
             : Status::Unavailable(std::string(operation) + " failed: " + std::to_string(result));
}

class OpenClFence final : public Fence {
 public:
  OpenClFence(cl_command_queue queue, cl_event event, std::vector<cl_mem> memory,
              std::vector<BufferPtr> outputs, std::size_t input_count)
      : queue_(queue),
        event_(event),
        memory_(std::move(memory)),
        outputs_(std::move(outputs)),
        input_count_(input_count) {
    clRetainCommandQueue(queue_);
  }
  ~OpenClFence() override {
    if (event_ != nullptr) clReleaseEvent(event_);
    for (cl_mem memory : memory_) clReleaseMemObject(memory);
    clReleaseCommandQueue(queue_);
  }

  Status Wait(std::int64_t timeout_ms) override {
    std::lock_guard<std::mutex> lock(mutex_);
    if (completed_) return status_;
    const auto start = std::chrono::steady_clock::now();
    for (;;) {
      cl_int execution = 0;
      cl_int result = clGetEventInfo(event_, CL_EVENT_COMMAND_EXECUTION_STATUS, sizeof(execution),
                                     &execution, nullptr);
      if (result != CL_SUCCESS) return status_ = OpenClStatus(result, "clGetEventInfo");
      if (execution == CL_COMPLETE) break;
      if (execution < 0) return status_ = Status::Unavailable("OpenCL kernel execution failed");
      if (timeout_ms >= 0 && std::chrono::duration_cast<std::chrono::milliseconds>(
                                 std::chrono::steady_clock::now() - start)
                                     .count() >= timeout_ms) {
        return Status::Unavailable("OpenCL fence wait timed out");
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    for (std::size_t i = 0; i < outputs_.size(); ++i) {
      if (!outputs_[i]) return status_ = Status::Invalid("OpenCL output buffer is null");
      status_ = outputs_[i]->BeginCpuAccess(MapAccess::kWrite);
      if (!status_.ok()) return status_;
      void* destination = outputs_[i]->Map(MapAccess::kWrite);
      if (destination == nullptr) {
        outputs_[i]->EndCpuAccess(MapAccess::kWrite);
        return status_ = Status::Invalid("OpenCL output buffer is not CPU mappable");
      }
      const cl_int result =
          clEnqueueReadBuffer(queue_, memory_[input_count_ + i], CL_TRUE, 0, outputs_[i]->size(),
                              destination, 0, nullptr, nullptr);
      outputs_[i]->Unmap();
      status_ = outputs_[i]->EndCpuAccess(MapAccess::kWrite);
      if (!status_.ok()) return status_;
      if (result != CL_SUCCESS) return status_ = OpenClStatus(result, "clEnqueueReadBuffer");
    }
    completed_ = true;
    status_ = Status::Ok();
    return status_;
  }

 private:
  cl_command_queue queue_ = nullptr;
  cl_event event_ = nullptr;
  std::vector<cl_mem> memory_;
  std::vector<BufferPtr> outputs_;
  std::size_t input_count_ = 0;
  std::mutex mutex_;
  bool completed_ = false;
  Status status_;
};

class OpenClBackend final : public IOpenClBackend {
 public:
  ~OpenClBackend() override { Close(); }

  Status Compile(const OpenClKernelConfig& config) override {
    if (config.name.empty() || config.source.empty())
      return Status::Invalid("OpenCL kernel name and source are required");
    std::lock_guard<std::mutex> lock(mutex_);
    Status status = InitializeLocked();
    if (!status.ok()) return status;
    const char* source = config.source.c_str();
    const std::size_t length = config.source.size();
    cl_int result = CL_SUCCESS;
    cl_program program = clCreateProgramWithSource(context_, 1, &source, &length, &result);
    if (result != CL_SUCCESS) return OpenClStatus(result, "clCreateProgramWithSource");
    result = clBuildProgram(program, 1, &device_, config.build_options.c_str(), nullptr, nullptr);
    if (result != CL_SUCCESS) {
      std::size_t log_size = 0;
      clGetProgramBuildInfo(program, device_, CL_PROGRAM_BUILD_LOG, 0, nullptr, &log_size);
      std::string log(log_size, '\0');
      if (log_size != 0)
        clGetProgramBuildInfo(program, device_, CL_PROGRAM_BUILD_LOG, log_size, log.data(),
                              nullptr);
      clReleaseProgram(program);
      return Status::Invalid("OpenCL program build failed: " + log);
    }
    cl_kernel kernel = clCreateKernel(program, config.name.c_str(), &result);
    if (result != CL_SUCCESS) {
      clReleaseProgram(program);
      return OpenClStatus(result, "clCreateKernel");
    }
    const auto existing = kernels_.find(config.name);
    if (existing != kernels_.end()) {
      clReleaseKernel(existing->second.kernel);
      clReleaseProgram(existing->second.program);
    }
    kernels_[config.name] = {program, kernel};
    return Status::Ok();
  }

  Status Run(const std::string& kernel_name, const std::vector<BufferPtr>& inputs,
             const std::vector<BufferPtr>& outputs,
             const std::vector<std::size_t>& global_work_size,
             std::shared_ptr<Fence>* fence) override {
    if (global_work_size.empty() || global_work_size.size() > 3)
      return Status::Invalid("OpenCL work size rank must be 1 to 3");
    std::lock_guard<std::mutex> lock(mutex_);
    Status status = InitializeLocked();
    if (!status.ok()) return status;
    const auto kernel = kernels_.find(kernel_name);
    if (kernel == kernels_.end())
      return Status::NotFound("OpenCL kernel is not compiled: " + kernel_name);

    std::vector<cl_mem> memory;
    memory.reserve(inputs.size() + outputs.size());
    cl_int result = CL_SUCCESS;
    for (const BufferPtr& input : inputs) {
      if (!input) {
        ReleaseMemory(memory);
        return Status::Invalid("OpenCL input buffer is null");
      }
      if (input->fence()) {
        status = input->fence()->Wait(-1);
        if (!status.ok()) {
          ReleaseMemory(memory);
          return status;
        }
      }
      status = input->BeginCpuAccess(MapAccess::kRead);
      if (!status.ok()) {
        ReleaseMemory(memory);
        return status;
      }
      void* data = input->Map(MapAccess::kRead);
      if (data == nullptr) {
        input->EndCpuAccess(MapAccess::kRead);
        ReleaseMemory(memory);
        return Status::Invalid("OpenCL input buffer is not CPU mappable");
      }
      cl_mem object = clCreateBuffer(context_, CL_MEM_READ_ONLY | CL_MEM_COPY_HOST_PTR,
                                     input->size(), data, &result);
      input->Unmap();
      status = input->EndCpuAccess(MapAccess::kRead);
      if (!status.ok()) {
        if (object != nullptr) clReleaseMemObject(object);
        ReleaseMemory(memory);
        return status;
      }
      if (result != CL_SUCCESS) {
        ReleaseMemory(memory);
        return OpenClStatus(result, "clCreateBuffer input");
      }
      memory.push_back(object);
    }
    for (const BufferPtr& output : outputs) {
      if (!output || output->size() == 0) {
        ReleaseMemory(memory);
        return Status::Invalid("OpenCL output buffer is invalid");
      }
      cl_mem object = clCreateBuffer(context_, CL_MEM_WRITE_ONLY, output->size(), nullptr, &result);
      if (result != CL_SUCCESS) {
        ReleaseMemory(memory);
        return OpenClStatus(result, "clCreateBuffer output");
      }
      memory.push_back(object);
    }
    for (std::size_t i = 0; i < memory.size(); ++i) {
      result = clSetKernelArg(kernel->second.kernel, static_cast<cl_uint>(i), sizeof(cl_mem),
                              &memory[i]);
      if (result != CL_SUCCESS) {
        ReleaseMemory(memory);
        return OpenClStatus(result, "clSetKernelArg");
      }
    }
    cl_event event = nullptr;
    result = clEnqueueNDRangeKernel(queue_, kernel->second.kernel,
                                    static_cast<cl_uint>(global_work_size.size()), nullptr,
                                    global_work_size.data(), nullptr, 0, nullptr, &event);
    if (result != CL_SUCCESS) {
      ReleaseMemory(memory);
      return OpenClStatus(result, "clEnqueueNDRangeKernel");
    }
    clFlush(queue_);
    auto completion =
        std::make_shared<OpenClFence>(queue_, event, std::move(memory), outputs, inputs.size());
    if (fence != nullptr) {
      *fence = std::move(completion);
      return Status::Ok();
    }
    return completion->Wait(-1);
  }

  void Close() override {
    std::lock_guard<std::mutex> lock(mutex_);
    for (const auto& kernel : kernels_) {
      clReleaseKernel(kernel.second.kernel);
      clReleaseProgram(kernel.second.program);
    }
    kernels_.clear();
    if (queue_ != nullptr) clReleaseCommandQueue(queue_);
    if (context_ != nullptr) clReleaseContext(context_);
    queue_ = nullptr;
    context_ = nullptr;
    device_ = nullptr;
  }

 private:
  struct CompiledKernel {
    cl_program program = nullptr;
    cl_kernel kernel = nullptr;
  };

  static void ReleaseMemory(const std::vector<cl_mem>& memory) {
    for (cl_mem object : memory) clReleaseMemObject(object);
  }

  Status InitializeLocked() {
    if (context_ != nullptr) return Status::Ok();
    cl_uint platform_count = 0;
    cl_int result = clGetPlatformIDs(0, nullptr, &platform_count);
    if (result != CL_SUCCESS || platform_count == 0)
      return Status::Unavailable("no OpenCL platform available");
    std::vector<cl_platform_id> platforms(platform_count);
    result = clGetPlatformIDs(platform_count, platforms.data(), nullptr);
    if (result != CL_SUCCESS) return OpenClStatus(result, "clGetPlatformIDs");
    for (cl_platform_id platform : platforms) {
      result = clGetDeviceIDs(platform, CL_DEVICE_TYPE_GPU, 1, &device_, nullptr);
      if (result == CL_SUCCESS) break;
      device_ = nullptr;
    }
    if (device_ == nullptr) return Status::Unavailable("no OpenCL GPU device available");
    context_ = clCreateContext(nullptr, 1, &device_, nullptr, nullptr, &result);
    if (result != CL_SUCCESS) return OpenClStatus(result, "clCreateContext");
    queue_ = clCreateCommandQueue(context_, device_, 0, &result);
    if (result != CL_SUCCESS) {
      clReleaseContext(context_);
      context_ = nullptr;
      return OpenClStatus(result, "clCreateCommandQueue");
    }
    return Status::Ok();
  }

  std::mutex mutex_;
  cl_device_id device_ = nullptr;
  cl_context context_ = nullptr;
  cl_command_queue queue_ = nullptr;
  std::unordered_map<std::string, CompiledKernel> kernels_;
};

}  // namespace

std::unique_ptr<IOpenClBackend> CreateOpenClBackend() { return std::make_unique<OpenClBackend>(); }

}  // namespace rkavp
