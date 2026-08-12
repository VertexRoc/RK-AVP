#pragma once

#include <cstdint>
#include <mutex>
#include <string>
#include <unordered_map>

namespace rkavp {

class MetricsRegistry {
 public:
  void Increment(const std::string& name, std::uint64_t amount = 1);
  void SetGauge(const std::string& name, double value);
  std::uint64_t Counter(const std::string& name) const;
  double Gauge(const std::string& name) const;
  std::string ExportText() const;

 private:
  mutable std::mutex mutex_;
  std::unordered_map<std::string, std::uint64_t> counters_;
  std::unordered_map<std::string, double> gauges_;
};

}  // namespace rkavp
