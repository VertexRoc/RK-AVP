#pragma once

#include <functional>
#include <string>

#include "rkavp/media_types.hpp"
#include "rkavp/status.hpp"

namespace rkavp {

struct ReconnectPolicy {
  int max_attempts = -1;
  int initial_delay_ms = 250;
  int maximum_delay_ms = 10000;
  double multiplier = 2.0;
};

struct StreamingSessionConfig {
  std::string url;
  std::string transport = "tcp";
  ReconnectPolicy reconnect;
};

class IStreamingInputBackend {
 public:
  virtual ~IStreamingInputBackend() = default;
  virtual Status Open(const StreamingSessionConfig& config) = 0;
  virtual Status Read(EncodedPacket* packet) = 0;
  virtual void RequestStop() = 0;
  virtual void Close() = 0;
};

class IStreamingOutputBackend {
 public:
  virtual ~IStreamingOutputBackend() = default;
  virtual Status Open(const StreamingSessionConfig& config) = 0;
  virtual Status Publish(const EncodedPacket& packet) = 0;
  virtual void SetKeyFrameRequestCallback(std::function<void()> callback) = 0;
  virtual void Close() = 0;
};

}  // namespace rkavp
