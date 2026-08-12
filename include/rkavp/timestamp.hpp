#pragma once

#include <cstdint>
#include <limits>
#include <stdexcept>

namespace rkavp {

struct TimeBase {
  std::int64_t numerator = 1;
  std::int64_t denominator = 1000000;

  bool valid() const { return numerator > 0 && denominator > 0; }
};

class Timestamp {
 public:
  static constexpr std::int64_t kUnsetValue = std::numeric_limits<std::int64_t>::min();
  static constexpr std::int64_t kDoneValue = std::numeric_limits<std::int64_t>::max();

  Timestamp() = default;
  explicit constexpr Timestamp(std::int64_t microseconds) : microseconds_(microseconds) {}

  static constexpr Timestamp Unset() { return Timestamp(kUnsetValue); }
  static constexpr Timestamp Done() { return Timestamp(kDoneValue); }
  static constexpr Timestamp FromMicroseconds(std::int64_t value) { return Timestamp(value); }
  static Timestamp FromTicks(std::int64_t ticks, TimeBase time_base) {
    if (!time_base.valid()) {
      throw std::invalid_argument("invalid time base");
    }
    const long double us =
        static_cast<long double>(ticks) * time_base.numerator * 1000000.0L / time_base.denominator;
    if (us > static_cast<long double>(kDoneValue - 1) ||
        us < static_cast<long double>(kUnsetValue + 1)) {
      throw std::overflow_error("timestamp conversion overflow");
    }
    return Timestamp(static_cast<std::int64_t>(us));
  }

  bool is_unset() const { return microseconds_ == kUnsetValue; }
  bool is_done() const { return microseconds_ == kDoneValue; }
  bool is_range_value() const { return !is_unset() && !is_done(); }
  std::int64_t microseconds() const { return microseconds_; }

  friend bool operator==(Timestamp lhs, Timestamp rhs) {
    return lhs.microseconds_ == rhs.microseconds_;
  }
  friend bool operator!=(Timestamp lhs, Timestamp rhs) { return !(lhs == rhs); }
  friend bool operator<(Timestamp lhs, Timestamp rhs) {
    return lhs.microseconds_ < rhs.microseconds_;
  }
  friend bool operator<=(Timestamp lhs, Timestamp rhs) {
    return lhs.microseconds_ <= rhs.microseconds_;
  }
  friend bool operator>(Timestamp lhs, Timestamp rhs) { return rhs < lhs; }
  friend bool operator>=(Timestamp lhs, Timestamp rhs) { return rhs <= lhs; }

 private:
  std::int64_t microseconds_ = kUnsetValue;
};

}  // namespace rkavp
