#include "rkavp/scheduler.hpp"

#include <algorithm>
#include <chrono>
#include <exception>

#if defined(__linux__)
#include <pthread.h>
#include <sched.h>
#endif

namespace rkavp {

// The public C++17 API predates strong option types; call sites name these values explicitly.
// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
Scheduler::Scheduler(std::size_t worker_count, std::size_t queue_capacity, std::string name,
                     int priority, std::shared_ptr<IClock> clock)
    : worker_count_(worker_count == 0 ? 1 : worker_count),
      queue_capacity_(queue_capacity == 0 ? 1 : queue_capacity),
      name_(std::move(name)),
      priority_(priority),
      clock_(clock ? std::move(clock) : std::make_shared<SteadyClock>()) {}

Scheduler::~Scheduler() { Stop(); }

Status Scheduler::Start() {
  bool expected = false;
  if (!running_.compare_exchange_strong(expected, true))
    return Status::AlreadyExists("scheduler is already running");
  drain_ = true;
  starvation_events_ = 0;
  max_wait_us_ = 0;
  {
    std::lock_guard<std::mutex> lock(error_mutex_);
    last_error_ = Status::Ok();
  }
  workers_.reserve(worker_count_);
  for (std::size_t i = 0; i < worker_count_; ++i)
    workers_.emplace_back(&Scheduler::WorkerLoop, this);
  return Status::Ok();
}

Status Scheduler::Post(std::function<void()> task) { return PostImpl(std::move(task), true); }

Status Scheduler::PostGuaranteed(std::function<void()> task) {
  return PostImpl(std::move(task), false);
}

Status Scheduler::PostImpl(std::function<void()> task, bool enforce_capacity) {
  if (!task) return Status::Invalid("scheduler task is empty");
  if (!running_) return Status::FailedPrecondition("scheduler is not running");
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!running_) return Status::FailedPrecondition("scheduler is not running");
    if (enforce_capacity && tasks_.size() >= queue_capacity_)
      return Status(StatusCode::kUnavailable, "executor queue is full: " + name_);
    tasks_.push({std::move(task), clock_->NowMicros()});
  }
  condition_.notify_one();
  return Status::Ok();
}

Status Scheduler::WaitIdle(std::int64_t timeout_ms) {
  std::unique_lock<std::mutex> lock(mutex_);
  const auto idle = [this] { return tasks_.empty() && active_tasks_ == 0; };
  if (timeout_ms < 0)
    idle_condition_.wait(lock, idle);
  else if (!idle_condition_.wait_for(lock, std::chrono::milliseconds(timeout_ms), idle)) {
    return Status(StatusCode::kUnavailable, "executor wait idle timed out: " + name_);
  }
  return last_error();
}

void Scheduler::Stop(bool drain) {
  if (!running_.exchange(false)) return;
  drain_ = drain;
  if (!drain) {
    std::lock_guard<std::mutex> lock(mutex_);
    std::queue<TaskEntry> empty;
    tasks_.swap(empty);
  }
  condition_.notify_all();
  for (auto& worker : workers_)
    if (worker.joinable()) worker.join();
  workers_.clear();
}

Status Scheduler::last_error() const {
  std::lock_guard<std::mutex> lock(error_mutex_);
  return last_error_;
}

std::size_t Scheduler::queued() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return tasks_.size();
}

std::size_t Scheduler::active() const { return active_tasks_.load(); }

void Scheduler::WorkerLoop() {
#if defined(__linux__)
  if (priority_ != 0) {
    sched_param parameter{};
    parameter.sched_priority = priority_;
    pthread_setschedparam(pthread_self(), SCHED_FIFO, &parameter);
  }
#endif
  while (true) {
    TaskEntry entry;
    std::unique_lock<std::mutex> lock(mutex_);
    condition_.wait(lock, [this] { return !running_ || !tasks_.empty(); });
    if (!running_ && (!drain_ || tasks_.empty())) return;
    if (tasks_.empty()) continue;
    entry = std::move(tasks_.front());
    tasks_.pop();
    ++active_tasks_;
    lock.unlock();
    const std::int64_t wait_us = std::max<std::int64_t>(0, clock_->NowMicros() - entry.enqueued_us);
    std::int64_t observed = max_wait_us_.load();
    while (wait_us > observed && !max_wait_us_.compare_exchange_weak(observed, wait_us)) {
    }
    if (wait_us >= kStarvationThresholdUs) ++starvation_events_;
    try {
      entry.task();
    } catch (const std::exception& error) {
      std::lock_guard<std::mutex> lock(error_mutex_);
      if (last_error_.ok())
        last_error_ = Status::Internal(std::string("scheduler task failed: ") + error.what());
    } catch (...) {
      std::lock_guard<std::mutex> lock(error_mutex_);
      if (last_error_.ok())
        last_error_ = Status::Internal("scheduler task failed with an unknown exception");
    }
    --active_tasks_;
    idle_condition_.notify_all();
  }
}

}  // namespace rkavp
