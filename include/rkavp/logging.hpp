#pragma once

#include <chrono>
#include <cstdint>
#include <functional>
#include <optional>
#include <ostream>
#include <sstream>
#include <string>
#include <thread>

namespace rkavp {

enum class LogLevel { kTrace = 0, kDebug, kInfo, kWarning, kError, kFatal };

struct LogContext {
  std::string graph;
  std::string node;
  std::string source;
  std::optional<std::uint64_t> frame_id;
  std::string executor;
  std::string stream;
  std::optional<std::int64_t> pts_us;
};

struct LogRecord {
  std::chrono::system_clock::time_point timestamp;
  LogLevel level = LogLevel::kInfo;
  std::thread::id thread_id;
  const char* file = "";
  int line = 0;
  std::string message;
  LogContext context;
};

struct LoggingOptions {
  LogLevel min_level = LogLevel::kInfo;
  bool include_source = true;
};

using LogSink = std::function<void(const LogRecord&)>;

void InitializeLogging(LoggingOptions options = {});
void SetMinLogLevel(LogLevel level);
LogLevel GetMinLogLevel();
LogLevel ParseLogLevel(const std::string& value, LogLevel fallback = LogLevel::kInfo);
const char* LogLevelName(LogLevel level);
bool ShouldLog(LogLevel level);
void SetLogSink(LogSink sink);
void ResetLogSink();
void WriteLog(const LogRecord& record);

class ScopedLogContext {
 public:
  explicit ScopedLogContext(LogContext context);
  ~ScopedLogContext();

  ScopedLogContext(const ScopedLogContext&) = delete;
  ScopedLogContext& operator=(const ScopedLogContext&) = delete;

 private:
  LogContext previous_;
};

class LogMessage {
 public:
  LogMessage(LogLevel level, const char* file, int line);
  ~LogMessage();

  std::ostream& stream() { return stream_; }

 private:
  LogLevel level_;
  const char* file_;
  int line_;
  std::ostringstream stream_;
};

class LogMessageVoidify {
 public:
  void operator&(std::ostream&) const {}
};

}  // namespace rkavp

#define RKAVP_LOG(level)                           \
  !::rkavp::ShouldLog(::rkavp::LogLevel::k##level) \
      ? (void)0                                    \
      : ::rkavp::LogMessageVoidify() &             \
            ::rkavp::LogMessage(::rkavp::LogLevel::k##level, __FILE__, __LINE__).stream()
