#include "rkavp/config_value.hpp"

#include <sstream>

namespace rkavp {

std::string ConfigValue::ToString() const {
  if (is_null()) return {};
  if (Is<bool>()) return As<bool>() ? "true" : "false";
  if (Is<std::int64_t>()) return std::to_string(As<std::int64_t>());
  if (Is<double>()) {
    std::ostringstream stream;
    stream << As<double>();
    return stream.str();
  }
  if (Is<std::string>()) return As<std::string>();
  return Is<std::shared_ptr<Array>>() ? "[array]" : "{object}";
}

}  // namespace rkavp
