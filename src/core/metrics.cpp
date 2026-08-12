#include "rkavp/metrics.hpp"

#include <sstream>

namespace rkavp {

void MetricsRegistry::Increment(const std::string& name, std::uint64_t amount) {
  std::lock_guard<std::mutex> lock(mutex_);
  counters_[name] += amount;
}

void MetricsRegistry::SetGauge(const std::string& name, double value) {
  std::lock_guard<std::mutex> lock(mutex_);
  gauges_[name] = value;
}

std::uint64_t MetricsRegistry::Counter(const std::string& name) const {
  std::lock_guard<std::mutex> lock(mutex_);
  const auto it = counters_.find(name);
  return it == counters_.end() ? 0 : it->second;
}

double MetricsRegistry::Gauge(const std::string& name) const {
  std::lock_guard<std::mutex> lock(mutex_);
  const auto it = gauges_.find(name);
  return it == gauges_.end() ? 0.0 : it->second;
}

std::string MetricsRegistry::ExportText() const {
  std::lock_guard<std::mutex> lock(mutex_);
  std::ostringstream output;
  for (const auto& item : counters_) output << item.first << " " << item.second << "\n";
  for (const auto& item : gauges_) output << item.first << " " << item.second << "\n";
  return output.str();
}

}  // namespace rkavp
