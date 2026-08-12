#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "rkavp/timestamp.hpp"

namespace rkavp {

struct NodeRuntimeInfo {
  std::string id;
  std::string executor;
  std::string state;
  std::size_t pending_packets = 0;
};

struct ExecutorRuntimeInfo {
  std::string name;
  std::size_t queued = 0;
  std::size_t active = 0;
  std::uint64_t starvation_events = 0;
  std::int64_t max_wait_us = 0;
};

struct StreamRuntimeInfo {
  std::string name;
  bool open = false;
  Timestamp last_timestamp = Timestamp::Unset();
  Timestamp bound = Timestamp::Unset();
  std::size_t queue_depth = 0;
  std::uint64_t dropped = 0;
};

struct GraphRuntimeInfo {
  bool running = false;
  bool cancelled = false;
  std::vector<NodeRuntimeInfo> nodes;
  std::vector<ExecutorRuntimeInfo> executors;
  std::vector<StreamRuntimeInfo> streams;
};

enum class StreamHealthState {
  kConnecting,
  kStreaming,
  kStalled,
  kReconnecting,
  kEos,
  kFailed,
  kStopped
};

struct StreamHealth {
  StreamHealthState state = StreamHealthState::kStopped;
  std::int64_t last_packet_time_us = 0;
  std::uint64_t reconnect_count = 0;
  std::uint64_t dropped_packets = 0;
  std::string message;
};

struct SourceInfo {
  std::string slot;
  std::string source_id;
  StreamHealth health;
};

}  // namespace rkavp
