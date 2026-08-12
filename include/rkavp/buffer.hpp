#pragma once

#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <utility>
#include <vector>

#include "rkavp/status.hpp"

namespace rkavp {

enum class MemoryType { kHost, kDmaBuf, kMpp, kRknn, kOpenCl };
enum class MapAccess { kRead, kWrite, kReadWrite };
enum class SyncDirection { kDeviceToCpu, kCpuToDevice };

struct BufferPlane {
  int fd = -1;
  std::size_t offset = 0;
  std::size_t size = 0;
  std::size_t row_stride = 0;
  std::size_t pixel_stride = 0;
  std::uint64_t modifier = 0;
};

class Fence {
 public:
  virtual ~Fence() = default;
  virtual Status Wait(std::int64_t timeout_ms) = 0;
};

class Buffer {
 public:
  virtual ~Buffer() = default;
  virtual MemoryType memory_type() const = 0;
  virtual std::size_t size() const = 0;
  virtual const std::vector<BufferPlane>& planes() const = 0;
  virtual Status BeginCpuAccess(MapAccess) { return Status::Ok(); }
  virtual Status EndCpuAccess(MapAccess) { return Status::Ok(); }
  virtual Status Sync(SyncDirection) { return Status::Ok(); }
  virtual void* Map(MapAccess) { return nullptr; }
  virtual void Unmap() {}
  void set_fence(std::shared_ptr<Fence> fence) { fence_ = std::move(fence); }
  const std::shared_ptr<Fence>& fence() const { return fence_; }

 private:
  std::shared_ptr<Fence> fence_;
};

class HostBuffer final : public Buffer {
 public:
  explicit HostBuffer(std::size_t size) : bytes_(size), planes_({{-1, 0, size, 0, 0, 0}}) {}
  explicit HostBuffer(std::vector<std::uint8_t> bytes)
      : bytes_(std::move(bytes)), planes_({{-1, 0, bytes_.size(), 0, 0, 0}}) {}

  MemoryType memory_type() const override { return MemoryType::kHost; }
  std::size_t size() const override { return bytes_.size(); }
  const std::vector<BufferPlane>& planes() const override { return planes_; }
  void* Map(MapAccess) override { return bytes_.data(); }
  std::uint8_t* data() { return bytes_.data(); }
  const std::uint8_t* data() const { return bytes_.data(); }

 private:
  std::vector<std::uint8_t> bytes_;
  std::vector<BufferPlane> planes_;
};

class DmaBuffer final : public Buffer {
 public:
  static std::shared_ptr<DmaBuffer> Adopt(int fd, std::size_t size,
                                          std::vector<BufferPlane> planes = {});
  static std::shared_ptr<DmaBuffer> Duplicate(int fd, std::size_t size,
                                              std::vector<BufferPlane> planes = {});
  ~DmaBuffer() override;

  DmaBuffer(const DmaBuffer&) = delete;
  DmaBuffer& operator=(const DmaBuffer&) = delete;

  MemoryType memory_type() const override { return MemoryType::kDmaBuf; }
  std::size_t size() const override { return size_; }
  const std::vector<BufferPlane>& planes() const override { return planes_; }
  int fd() const { return fd_; }
  Status BeginCpuAccess(MapAccess access) override;
  Status EndCpuAccess(MapAccess access) override;
  Status Sync(SyncDirection direction) override;
  void* Map(MapAccess access) override;
  void Unmap() override;

 private:
  DmaBuffer(int fd, std::size_t size, std::vector<BufferPlane> planes);
  int fd_ = -1;
  std::size_t size_ = 0;
  std::vector<BufferPlane> planes_;
  std::vector<int> owned_fds_;
  void* mapped_ = nullptr;
};

using BufferPtr = std::shared_ptr<Buffer>;

class BufferAllocator {
 public:
  virtual ~BufferAllocator() = default;
  virtual Status Allocate(std::size_t size, BufferPtr* buffer) = 0;
};

class HostBufferAllocator final : public BufferAllocator {
 public:
  Status Allocate(std::size_t size, BufferPtr* buffer) override;
};

class BufferPool {
 public:
  BufferPool(std::shared_ptr<BufferAllocator> allocator, std::size_t buffer_size,
             std::size_t capacity);
  Status Acquire(BufferPtr* buffer, std::int64_t timeout_ms = -1);
  Status Release(BufferPtr buffer);
  void Close();
  std::size_t available() const;
  std::size_t capacity() const { return capacity_; }

 private:
  std::shared_ptr<BufferAllocator> allocator_;
  std::size_t buffer_size_;
  std::size_t capacity_;
  mutable std::mutex mutex_;
  std::condition_variable condition_;
  std::deque<BufferPtr> available_;
  std::size_t allocated_ = 0;
  bool closed_ = false;
};

}  // namespace rkavp
