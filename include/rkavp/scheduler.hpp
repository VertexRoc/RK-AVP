#pragma once

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <functional>
#include <memory>
#include <mutex>
#include <queue>
#include <string>
#include <thread>
#include <vector>

#include "rkavp/clock.hpp"
#include "rkavp/status.hpp"

namespace rkavp {

class Scheduler {
 public:
  explicit Scheduler(std::size_t worker_count = 1, std::size_t queue_capacity = 256,
                     std::string name = "default", int priority = 0,
                     std::shared_ptr<IClock> clock = std::make_shared<SteadyClock>());
  ~Scheduler();

  Status Start();
  Status Post(std::function<void()> task);
  // Runtime drain tokens are bounded by the number of graph streams, so they must not be
  // rejected merely because the user-task queue is temporarily full.
  Status PostGuaranteed(std::function<void()> task);
  Status WaitIdle(std::int64_t timeout_ms = -1);
  void Stop(bool drain = true);
  bool running() const { return running_; }
  Status last_error() const;
  std::size_t queued() const;
  std::size_t active() const;
  std::uint64_t starvation_events() const { return starvation_events_; }
  std::int64_t max_wait_us() const { return max_wait_us_; }
  const std::string& name() const { return name_; }

 private:
  Status PostImpl(std::function<void()> task, bool enforce_capacity);
  void WorkerLoop();
  struct TaskEntry {
    std::function<void()> task;
    std::int64_t enqueued_us = 0;
  };
  std::size_t worker_count_;
  std::size_t queue_capacity_;
  std::string name_;
  int priority_;
  std::atomic<bool> running_{false};
  std::atomic<bool> drain_{true};
  mutable std::mutex mutex_;
  std::condition_variable condition_;
  std::condition_variable idle_condition_;
  std::queue<TaskEntry> tasks_;
  std::atomic<std::size_t> active_tasks_{0};
  std::vector<std::thread> workers_;
  mutable std::mutex error_mutex_;
  Status last_error_;
  std::shared_ptr<IClock> clock_;
  std::atomic<std::uint64_t> starvation_events_{0};
  std::atomic<std::int64_t> max_wait_us_{0};
  static constexpr std::int64_t kStarvationThresholdUs = 100000;
};

}  // namespace rkavp
