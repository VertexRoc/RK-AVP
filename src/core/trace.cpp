#include "rkavp/trace.hpp"

#include <sstream>
#include <utility>

namespace rkavp {
namespace {
std::string Escape(const std::string& value) {
  std::string result;
  for (char ch : value) {
    if (ch == '"' || ch == '\\') result.push_back('\\');
    result.push_back(ch);
  }
  return result;
}
}  // namespace

TraceBuffer::TraceBuffer(std::size_t capacity) : capacity_(capacity == 0 ? 1 : capacity) {
  events_.reserve(capacity_);
}

void TraceBuffer::Add(TraceEvent event) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (events_.size() < capacity_) {
    events_.push_back(std::move(event));
    return;
  }
  events_[next_] = std::move(event);
  next_ = (next_ + 1) % capacity_;
  wrapped_ = true;
}

std::vector<TraceEvent> TraceBuffer::Snapshot() const {
  std::lock_guard<std::mutex> lock(mutex_);
  if (!wrapped_) return events_;
  std::vector<TraceEvent> result;
  result.reserve(events_.size());
  result.insert(result.end(), events_.begin() + static_cast<std::ptrdiff_t>(next_), events_.end());
  result.insert(result.end(), events_.begin(),
                events_.begin() + static_cast<std::ptrdiff_t>(next_));
  return result;
}

std::string TraceBuffer::ToChromeTraceJson() const {
  const auto events = Snapshot();
  std::ostringstream output;
  output << "{\"traceEvents\":[";
  for (std::size_t i = 0; i < events.size(); ++i) {
    if (i != 0) output << ',';
    const auto& event = events[i];
    output << "{\"name\":\"" << Escape(event.name) << "\",\"cat\":\"" << Escape(event.category)
           << "\",\"ph\":\"X\",\"ts\":" << event.timestamp_us << ",\"dur\":" << event.duration_us
           << ",\"pid\":1,\"tid\":" << event.thread_id << ",\"args\":{\"node\":\""
           << Escape(event.node) << "\",\"executor\":\"" << Escape(event.executor) << "\"}}";
  }
  output << "]}";
  return output.str();
}

}  // namespace rkavp
