#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <unordered_map>
#include <variant>

namespace rkavp {

using MetadataValue = std::variant<std::int64_t, double, bool, std::string>;

class MetadataSet {
 public:
  void Set(std::string key, MetadataValue value) { values_[std::move(key)] = std::move(value); }
  bool Contains(const std::string& key) const { return values_.find(key) != values_.end(); }
  std::size_t size() const { return values_.size(); }

  template <typename T>
  std::optional<T> Get(const std::string& key) const {
    const auto it = values_.find(key);
    if (it == values_.end()) {
      return std::nullopt;
    }
    if (const auto* value = std::get_if<T>(&it->second)) {
      return *value;
    }
    return std::nullopt;
  }

 private:
  std::unordered_map<std::string, MetadataValue> values_;
};

}  // namespace rkavp
