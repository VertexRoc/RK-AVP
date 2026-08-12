#pragma once

#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <typeindex>
#include <unordered_map>

#include "rkavp/buffer.hpp"

namespace rkavp {

class HardwareContextService {
 public:
  explicit HardwareContextService(
      std::shared_ptr<BufferAllocator> allocator = std::make_shared<HostBufferAllocator>())
      : allocator_(std::move(allocator)) {}

  std::shared_ptr<BufferAllocator> allocator() const { return allocator_; }
  Status GetOrCreatePool(const std::string& name, std::size_t buffer_size, std::size_t capacity,
                         std::shared_ptr<BufferPool>* pool) {
    if (name.empty() || pool == nullptr || buffer_size == 0 || capacity == 0) {
      return Status::Invalid("buffer pool name, size, capacity and output are required");
    }
    std::lock_guard<std::mutex> lock(mutex_);
    auto& value = pools_[name];
    if (!value) value = std::make_shared<BufferPool>(allocator_, buffer_size, capacity);
    *pool = value;
    return Status::Ok();
  }
  template <typename T>
  Status SetBackendContext(const std::string& backend, std::shared_ptr<T> context) {
    if (backend.empty() || !context) return Status::Invalid("backend context is required");
    std::lock_guard<std::mutex> lock(mutex_);
    contexts_.insert_or_assign(backend,
                               ContextEntry{std::type_index(typeid(T)), std::move(context)});
    return Status::Ok();
  }
  template <typename T>
  std::shared_ptr<T> GetBackendContext(const std::string& backend) const {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto it = contexts_.find(backend);
    if (it == contexts_.end() || it->second.type != std::type_index(typeid(T))) return {};
    return std::static_pointer_cast<T>(it->second.value);
  }
  template <typename T>
  Status CacheImport(const std::string& backend, int fd, std::shared_ptr<T> handle) {
    if (backend.empty() || fd < 0 || !handle)
      return Status::Invalid("DMA import key and handle are required");
    std::lock_guard<std::mutex> lock(mutex_);
    const std::string key = backend + ":" + std::to_string(fd);
    imports_.try_emplace(key, 1);
    import_handles_.insert_or_assign(key,
                                     ContextEntry{std::type_index(typeid(T)), std::move(handle)});
    return Status::Ok();
  }
  template <typename T>
  std::shared_ptr<T> FindImport(const std::string& backend, int fd) const {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto it = import_handles_.find(backend + ":" + std::to_string(fd));
    if (it == import_handles_.end() || it->second.type != std::type_index(typeid(T))) return {};
    return std::static_pointer_cast<T>(it->second.value);
  }
  Status WaitFence(const BufferPtr& buffer, std::int64_t timeout_ms) const {
    if (!buffer) return Status::Invalid("buffer is required");
    return buffer->fence() ? buffer->fence()->Wait(timeout_ms) : Status::Ok();
  }
  Status Sync(const BufferPtr& buffer, SyncDirection direction) const {
    return buffer ? buffer->Sync(direction) : Status::Invalid("buffer is required");
  }
  Status BeginCpuAccess(const BufferPtr& buffer, MapAccess access) const {
    return buffer ? buffer->BeginCpuAccess(access) : Status::Invalid("buffer is required");
  }
  Status EndCpuAccess(const BufferPtr& buffer, MapAccess access) const {
    return buffer ? buffer->EndCpuAccess(access) : Status::Invalid("buffer is required");
  }
  void RecordImport(const std::string& backend, int fd) {
    std::lock_guard<std::mutex> lock(mutex_);
    ++imports_[backend + ":" + std::to_string(fd)];
  }
  std::uint64_t ImportCount(const std::string& backend, int fd) const {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto it = imports_.find(backend + ":" + std::to_string(fd));
    return it == imports_.end() ? 0 : it->second;
  }

 private:
  struct ContextEntry {
    std::type_index type;
    std::shared_ptr<void> value;
  };
  std::shared_ptr<BufferAllocator> allocator_;
  mutable std::mutex mutex_;
  std::unordered_map<std::string, std::uint64_t> imports_;
  std::unordered_map<std::string, ContextEntry> contexts_;
  std::unordered_map<std::string, ContextEntry> import_handles_;
  std::unordered_map<std::string, std::shared_ptr<BufferPool>> pools_;
};

}  // namespace rkavp
