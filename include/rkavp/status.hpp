#pragma once

#include <string>
#include <utility>

namespace rkavp {

enum class StatusCode {
  kOk = 0,
  kInvalidArgument,
  kNotFound,
  kAlreadyExists,
  kFailedPrecondition,
  kUnavailable,
  kInternal,
  kCancelled,
};

class Status {
 public:
  Status() = default;
  Status(StatusCode code, std::string message) : code_(code), message_(std::move(message)) {}

  static Status Ok() { return {}; }
  static Status Invalid(std::string message) {
    return {StatusCode::kInvalidArgument, std::move(message)};
  }
  static Status NotFound(std::string message) {
    return {StatusCode::kNotFound, std::move(message)};
  }
  static Status AlreadyExists(std::string message) {
    return {StatusCode::kAlreadyExists, std::move(message)};
  }
  static Status FailedPrecondition(std::string message) {
    return {StatusCode::kFailedPrecondition, std::move(message)};
  }
  static Status Internal(std::string message) {
    return {StatusCode::kInternal, std::move(message)};
  }
  static Status Cancelled(std::string message) {
    return {StatusCode::kCancelled, std::move(message)};
  }
  static Status Unavailable(std::string message) {
    return {StatusCode::kUnavailable, std::move(message)};
  }

  bool ok() const { return code_ == StatusCode::kOk; }
  explicit operator bool() const { return ok(); }
  StatusCode code() const { return code_; }
  const std::string& message() const { return message_; }

 private:
  StatusCode code_ = StatusCode::kOk;
  std::string message_;
};

}  // namespace rkavp
