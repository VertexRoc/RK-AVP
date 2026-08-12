#pragma once

#include <chrono>
#include <cstdint>
#include <mutex>

namespace rkavp {

class IClock {
 public:
  virtual ~IClock() = default;
  virtual std::int64_t NowMicros() const = 0;
};

class SteadyClock final : public IClock {
 public:
  std::int64_t NowMicros() const override {
    return std::chrono::duration_cast<std::chrono::microseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
  }
};

class FakeClock final : public IClock {
 public:
  explicit FakeClock(std::int64_t now_us = 0) : now_us_(now_us) {}
  std::int64_t NowMicros() const override {
    std::lock_guard<std::mutex> lock(mutex_);
    return now_us_;
  }
  void AdvanceMicros(std::int64_t amount) {
    std::lock_guard<std::mutex> lock(mutex_);
    now_us_ += amount;
  }
  void SetMicros(std::int64_t value) {
    std::lock_guard<std::mutex> lock(mutex_);
    now_us_ = value;
  }

 private:
  mutable std::mutex mutex_;
  std::int64_t now_us_ = 0;
};

}  // namespace rkavp
