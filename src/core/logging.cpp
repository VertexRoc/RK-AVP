#include "rkavp/logging.hpp"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstdlib>
#include <ctime>
#include <iomanip>
#include <iostream>
#include <mutex>
#include <utility>

namespace rkavp {
namespace {

std::atomic<LogLevel> g_min_level{LogLevel::kInfo};
std::atomic<bool> g_include_source{true};
std::mutex g_sink_mutex;
LogSink g_sink;
thread_local LogContext g_context;

std::string BaseName(const char* file) {
  const std::string path = file == nullptr ? "" : file;
  const std::size_t separator = path.find_last_of("/\\");
  return separator == std::string::npos ? path : path.substr(separator + 1);
}

std::string TimestampText(std::chrono::system_clock::time_point timestamp) {
  const auto time = std::chrono::system_clock::to_time_t(timestamp);
  std::tm utc{};
  ::gmtime_r(&time, &utc);
  const auto milliseconds =
      std::chrono::duration_cast<std::chrono::milliseconds>(timestamp.time_since_epoch()) %
      std::chrono::seconds(1);
  std::ostringstream output;
  output << std::put_time(&utc, "%Y-%m-%dT%H:%M:%S") << '.' << std::setw(3) << std::setfill('0')
         << milliseconds.count() << 'Z';
  return output.str();
}

void DefaultSink(const LogRecord& record) {
  std::ostringstream output;
  output << TimestampText(record.timestamp) << ' ' << LogLevelName(record.level)
         << " tid=" << record.thread_id;
  if (!record.context.graph.empty()) output << " graph=" << record.context.graph;
  if (!record.context.node.empty()) output << " node=" << record.context.node;
  if (!record.context.source.empty()) output << " source=" << record.context.source;
  if (record.context.frame_id) output << " frame=" << *record.context.frame_id;
  if (!record.context.executor.empty()) output << " executor=" << record.context.executor;
  if (!record.context.stream.empty()) output << " stream=" << record.context.stream;
  if (record.context.pts_us) output << " pts_us=" << *record.context.pts_us;
  if (g_include_source.load()) output << ' ' << BaseName(record.file) << ':' << record.line;
  output << " | " << record.message << '\n';
  std::cerr << output.str();
}

}  // namespace

void InitializeLogging(LoggingOptions options) {
  g_min_level.store(options.min_level);
  g_include_source.store(options.include_source);
}

void SetMinLogLevel(LogLevel level) { g_min_level.store(level); }

LogLevel GetMinLogLevel() { return g_min_level.load(); }

LogLevel ParseLogLevel(const std::string& value, LogLevel fallback) {
  std::string normalized = value;
  std::transform(
      normalized.begin(), normalized.end(), normalized.begin(),
      [](unsigned char character) { return static_cast<char>(std::tolower(character)); });
  if (normalized == "trace") return LogLevel::kTrace;
  if (normalized == "debug") return LogLevel::kDebug;
  if (normalized == "info") return LogLevel::kInfo;
  if (normalized == "warning" || normalized == "warn") return LogLevel::kWarning;
  if (normalized == "error") return LogLevel::kError;
  if (normalized == "fatal") return LogLevel::kFatal;
  return fallback;
}

const char* LogLevelName(LogLevel level) {
  switch (level) {
    case LogLevel::kTrace:
      return "TRACE";
    case LogLevel::kDebug:
      return "DEBUG";
    case LogLevel::kInfo:
      return "INFO";
    case LogLevel::kWarning:
      return "WARNING";
    case LogLevel::kError:
      return "ERROR";
    case LogLevel::kFatal:
      return "FATAL";
  }
  return "UNKNOWN";
}

bool ShouldLog(LogLevel level) {
  return static_cast<int>(level) >= static_cast<int>(g_min_level.load());
}

void SetLogSink(LogSink sink) {
  std::lock_guard<std::mutex> lock(g_sink_mutex);
  g_sink = std::move(sink);
}

void ResetLogSink() {
  std::lock_guard<std::mutex> lock(g_sink_mutex);
  g_sink = {};
}

void WriteLog(const LogRecord& record) {
  if (!ShouldLog(record.level)) return;
  LogSink sink;
  {
    std::lock_guard<std::mutex> lock(g_sink_mutex);
    sink = g_sink;
  }
  if (sink)
    sink(record);
  else
    DefaultSink(record);
  if (record.level == LogLevel::kFatal) std::abort();
}

ScopedLogContext::ScopedLogContext(LogContext context) : previous_(g_context) {
  g_context = std::move(context);
}

ScopedLogContext::~ScopedLogContext() { g_context = std::move(previous_); }

LogMessage::LogMessage(LogLevel level, const char* file, int line)
    : level_(level), file_(file), line_(line) {}

LogMessage::~LogMessage() {
  WriteLog({std::chrono::system_clock::now(), level_, std::this_thread::get_id(), file_, line_,
            stream_.str(), g_context});
}

}  // namespace rkavp
