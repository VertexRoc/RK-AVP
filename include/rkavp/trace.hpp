#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

namespace rkavp {

struct TraceEvent {
  std::string name;
  std::string category;
  std::string node;
  std::string executor;
  std::int64_t timestamp_us = 0;
  std::int64_t duration_us = 0;
  std::uint64_t thread_id = 0;
};

class TraceBuffer {
 public:
  explicit TraceBuffer(std::size_t capacity = 4096);
  void Add(TraceEvent event);
  std::vector<TraceEvent> Snapshot() const;
  std::string ToChromeTraceJson() const;

 private:
  std::size_t capacity_;
  mutable std::mutex mutex_;
  std::vector<TraceEvent> events_;
  std::size_t next_ = 0;
  bool wrapped_ = false;
};

}  // namespace rkavp
