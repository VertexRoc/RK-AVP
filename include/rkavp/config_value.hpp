#pragma once

#include <cstdint>
#include <memory>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <utility>
#include <variant>
#include <vector>

namespace rkavp {

class ConfigValue {
 public:
  using Array = std::vector<ConfigValue>;
  using Object = std::unordered_map<std::string, ConfigValue>;
  using Storage = std::variant<std::monostate, bool, std::int64_t, double, std::string,
                               std::shared_ptr<Array>, std::shared_ptr<Object>>;

  ConfigValue() = default;
  ConfigValue(bool value) : value_(value) {}
  ConfigValue(int value) : value_(static_cast<std::int64_t>(value)) {}
  ConfigValue(std::int64_t value) : value_(value) {}
  ConfigValue(double value) : value_(value) {}
  ConfigValue(const char* value) : value_(std::string(value == nullptr ? "" : value)) {}
  ConfigValue(std::string value) : value_(std::move(value)) {}
  ConfigValue(Array value) : value_(std::make_shared<Array>(std::move(value))) {}
  ConfigValue(Object value) : value_(std::make_shared<Object>(std::move(value))) {}

  bool is_null() const { return std::holds_alternative<std::monostate>(value_); }

  template <typename T>
  bool Is() const {
    return std::holds_alternative<T>(value_);
  }

  template <typename T>
  const T& As() const {
    return std::get<T>(value_);
  }

  const Array& AsArray() const { return *std::get<std::shared_ptr<Array>>(value_); }
  const Object& AsObject() const { return *std::get<std::shared_ptr<Object>>(value_); }
  std::string ToString() const;

 private:
  Storage value_;
};

}  // namespace rkavp
