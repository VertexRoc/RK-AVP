#include "rkavp/buffer.hpp"

#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

#include <cerrno>
#include <chrono>
#include <cstring>
#include <unordered_map>
#include <unordered_set>

namespace rkavp {
namespace {

constexpr std::uint64_t kSyncRead = 1ULL << 0;
constexpr std::uint64_t kSyncWrite = 2ULL << 0;
constexpr std::uint64_t kSyncStart = 0ULL << 2;
constexpr std::uint64_t kSyncEnd = 1ULL << 2;
struct DmaBufSync {
  std::uint64_t flags;
};
#ifndef DMA_BUF_IOCTL_SYNC
#define DMA_BUF_IOCTL_SYNC _IOW('b', 0, struct DmaBufSync)
#endif

std::uint64_t AccessFlags(MapAccess access) {
  switch (access) {
    case MapAccess::kRead:
      return kSyncRead;
    case MapAccess::kWrite:
      return kSyncWrite;
    case MapAccess::kReadWrite:
      return kSyncRead | kSyncWrite;
  }
  return kSyncRead | kSyncWrite;
}

Status SyncFd(int fd, MapAccess access, std::uint64_t phase) {
  DmaBufSync sync{AccessFlags(access) | phase};
  if (::ioctl(fd, DMA_BUF_IOCTL_SYNC, &sync) != 0) {
    return Status::Internal(std::string("DMA-BUF sync failed: ") + std::strerror(errno));
  }
  return Status::Ok();
}

}  // namespace

DmaBuffer::DmaBuffer(int fd, std::size_t size, std::vector<BufferPlane> planes)
    : fd_(fd), size_(size), planes_(std::move(planes)) {
  if (planes_.empty()) planes_.push_back({fd_, 0, size_, 0, 0, 0});
  for (auto& plane : planes_)
    if (plane.fd < 0) plane.fd = fd_;
  std::unordered_set<int> unique_fds;
  unique_fds.insert(fd_);
  for (const auto& plane : planes_)
    if (plane.fd >= 0) unique_fds.insert(plane.fd);
  owned_fds_.assign(unique_fds.begin(), unique_fds.end());
}

std::shared_ptr<DmaBuffer> DmaBuffer::Adopt(int fd, std::size_t size,
                                            std::vector<BufferPlane> planes) {
  if (fd < 0) throw std::invalid_argument("cannot adopt invalid dma-buf fd");
  return std::shared_ptr<DmaBuffer>(new DmaBuffer(fd, size, std::move(planes)));
}

std::shared_ptr<DmaBuffer> DmaBuffer::Duplicate(int fd, std::size_t size,
                                                std::vector<BufferPlane> planes) {
  if (fd < 0) throw std::invalid_argument("cannot duplicate invalid dma-buf fd");
  std::unordered_map<int, int> duplicates;
  auto duplicate_fd = [&duplicates](int source) {
    const auto existing = duplicates.find(source);
    if (existing != duplicates.end()) return existing->second;
    const int duplicate = ::fcntl(source, F_DUPFD_CLOEXEC, 0);
    if (duplicate >= 0) duplicates.emplace(source, duplicate);
    return duplicate;
  };
  const int owned_fd = duplicate_fd(fd);
  if (owned_fd < 0) throw std::runtime_error("dup failed for dma-buf fd");
  for (auto& plane : planes) {
    const int source = plane.fd < 0 ? fd : plane.fd;
    plane.fd = duplicate_fd(source);
    if (plane.fd < 0) {
      for (const auto& duplicate : duplicates) ::close(duplicate.second);
      throw std::runtime_error("dup failed for dma-buf plane fd");
    }
  }
  return Adopt(owned_fd, size, std::move(planes));
}

DmaBuffer::~DmaBuffer() {
  Unmap();
  for (int fd : owned_fds_)
    if (fd >= 0) ::close(fd);
}

Status DmaBuffer::BeginCpuAccess(MapAccess access) {
  std::size_t started = 0;
  for (; started < owned_fds_.size(); ++started) {
    Status status = SyncFd(owned_fds_[started], access, kSyncStart);
    if (status.ok()) continue;
    while (started != 0) SyncFd(owned_fds_[--started], access, kSyncEnd);
    return status;
  }
  return Status::Ok();
}

Status DmaBuffer::EndCpuAccess(MapAccess access) {
  Status first_error = Status::Ok();
  for (int fd : owned_fds_) {
    Status status = SyncFd(fd, access, kSyncEnd);
    if (!status.ok() && first_error.ok()) first_error = std::move(status);
  }
  return first_error;
}

Status DmaBuffer::Sync(SyncDirection direction) {
  return direction == SyncDirection::kDeviceToCpu ? BeginCpuAccess(MapAccess::kReadWrite)
                                                  : EndCpuAccess(MapAccess::kReadWrite);
}

void* DmaBuffer::Map(MapAccess access) {
  if (mapped_ != nullptr) return mapped_;
  int protection = PROT_READ;
  if (access != MapAccess::kRead) protection |= PROT_WRITE;
  mapped_ = ::mmap(nullptr, size_, protection, MAP_SHARED, fd_, 0);
  if (mapped_ == MAP_FAILED) mapped_ = nullptr;
  return mapped_;
}

void DmaBuffer::Unmap() {
  if (mapped_ != nullptr) {
    ::munmap(mapped_, size_);
    mapped_ = nullptr;
  }
}

Status HostBufferAllocator::Allocate(std::size_t size, BufferPtr* buffer) {
  if (buffer == nullptr || size == 0)
    return Status::Invalid("buffer output and positive size are required");
  *buffer = std::make_shared<HostBuffer>(size);
  return Status::Ok();
}

BufferPool::BufferPool(std::shared_ptr<BufferAllocator> allocator, std::size_t buffer_size,
                       std::size_t capacity)
    : allocator_(std::move(allocator)), buffer_size_(buffer_size), capacity_(capacity) {}

Status BufferPool::Acquire(BufferPtr* buffer, std::int64_t timeout_ms) {
  if (buffer == nullptr || !allocator_ || capacity_ == 0 || buffer_size_ == 0) {
    return Status::Invalid("invalid buffer pool configuration");
  }
  std::unique_lock<std::mutex> lock(mutex_);
  if (!available_.empty()) {
    *buffer = std::move(available_.front());
    available_.pop_front();
    return Status::Ok();
  }
  if (allocated_ < capacity_) {
    ++allocated_;
    lock.unlock();
    Status status = allocator_->Allocate(buffer_size_, buffer);
    if (!status.ok()) {
      lock.lock();
      --allocated_;
    }
    return status;
  }
  const auto ready = [this] { return closed_ || !available_.empty(); };
  bool signaled = true;
  if (timeout_ms < 0)
    condition_.wait(lock, ready);
  else
    signaled = condition_.wait_for(lock, std::chrono::milliseconds(timeout_ms), ready);
  if (!signaled) return Status(StatusCode::kUnavailable, "buffer pool acquire timed out");
  if (closed_) return Status::Cancelled("buffer pool is closed");
  *buffer = std::move(available_.front());
  available_.pop_front();
  return Status::Ok();
}

Status BufferPool::Release(BufferPtr buffer) {
  if (!buffer) return Status::Invalid("cannot release an empty buffer");
  std::lock_guard<std::mutex> lock(mutex_);
  if (closed_) return Status::Cancelled("buffer pool is closed");
  available_.push_back(std::move(buffer));
  condition_.notify_one();
  return Status::Ok();
}

void BufferPool::Close() {
  std::lock_guard<std::mutex> lock(mutex_);
  closed_ = true;
  available_.clear();
  condition_.notify_all();
}

std::size_t BufferPool::available() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return available_.size();
}

}  // namespace rkavp
