#pragma once

#include <condition_variable>
#include <cstddef>
#include <deque>
#include <mutex>
#include <utility>

namespace rkavp {

enum class QueuePolicy { kBlock, kDropOldest, kDropNewest };

template <typename T>
class BoundedQueue {
 public:
  BoundedQueue(std::size_t capacity, QueuePolicy policy) : capacity_(capacity), policy_(policy) {}

  bool Push(T value, bool* dropped = nullptr) {
    std::unique_lock<std::mutex> lock(mutex_);
    if (dropped) {
      *dropped = false;
    }
    if (capacity_ == 0 || closed_) {
      return false;
    }
    if (policy_ == QueuePolicy::kBlock) {
      not_full_.wait(lock, [this] { return closed_ || queue_.size() < capacity_; });
      if (closed_) {
        return false;
      }
    } else if (queue_.size() >= capacity_) {
      if (policy_ == QueuePolicy::kDropNewest) {
        if (dropped) {
          *dropped = true;
        }
        return false;
      }
      queue_.pop_front();
      if (dropped) {
        *dropped = true;
      }
    }
    queue_.push_back(std::move(value));
    not_empty_.notify_one();
    return true;
  }

  bool PushControl(T value, bool* dropped = nullptr) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (dropped) *dropped = false;
    if (capacity_ == 0 || closed_) return false;
    if (queue_.size() >= capacity_) {
      queue_.pop_front();
      if (dropped) *dropped = true;
    }
    queue_.push_back(std::move(value));
    not_empty_.notify_one();
    return true;
  }

  bool WaitPop(T* value) {
    std::unique_lock<std::mutex> lock(mutex_);
    not_empty_.wait(lock, [this] { return closed_ || !queue_.empty(); });
    if (queue_.empty()) {
      return false;
    }
    *value = std::move(queue_.front());
    queue_.pop_front();
    not_full_.notify_one();
    return true;
  }

  bool TryPop(T* value) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (value == nullptr || queue_.empty()) return false;
    *value = std::move(queue_.front());
    queue_.pop_front();
    not_full_.notify_one();
    return true;
  }

  void Close() {
    std::lock_guard<std::mutex> lock(mutex_);
    closed_ = true;
    not_empty_.notify_all();
    not_full_.notify_all();
  }

  std::size_t size() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return queue_.size();
  }

  bool closed() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return closed_;
  }

  std::size_t capacity() const { return capacity_; }
  QueuePolicy policy() const { return policy_; }

 private:
  const std::size_t capacity_;
  const QueuePolicy policy_;
  mutable std::mutex mutex_;
  std::condition_variable not_empty_;
  std::condition_variable not_full_;
  std::deque<T> queue_;
  bool closed_ = false;
};

}  // namespace rkavp
