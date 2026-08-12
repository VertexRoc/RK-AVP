#pragma once

#include <cstddef>
#include <string>
#include <unordered_map>
#include <vector>

#include "rkavp/bounded_queue.hpp"
#include "rkavp/config_value.hpp"
#include "rkavp/node.hpp"
#include "rkavp/status.hpp"

namespace rkavp {

struct QueueConfig {
  std::size_t capacity = 2;
  QueuePolicy policy = QueuePolicy::kDropOldest;
};

struct FlowControlConfig {
  QueueConfig input_queue{8, QueuePolicy::kBlock};
  QueueConfig edge_queue{2, QueuePolicy::kDropOldest};
  QueueConfig observer_queue{64, QueuePolicy::kDropOldest};
  bool observe_timestamp_bounds = false;
};

struct ExecutorConfig {
  std::string name = "default";
  std::size_t threads = 1;
  std::size_t queue_capacity = 256;
  int priority = 0;
};

struct NodeConfig {
  std::string id;
  std::string type;
  std::string executor = "default";
  NodeOptions options;
  bool has_input_policy = false;
  InputPolicy input_policy = InputPolicy::kAny;
  std::string trigger_port;
};

struct EdgeConfig {
  std::string from_node;
  std::string from_port;
  std::string to_node;
  std::string to_port;
  QueueConfig queue;
  bool back_edge = false;
  ConfigValue initial_packet;
};

struct GraphPortConfig {
  std::string name;
  std::string node;
  std::string port;
  QueueConfig queue{8, QueuePolicy::kBlock};
};

struct SourceSlotConfig {
  std::string name;
  std::string input_stream;
  std::string subgraph_template;
  std::string output_stream;
};

struct GraphConfig {
  int version = 1;
  std::string name;
  std::vector<ExecutorConfig> executors{{}};
  std::vector<NodeConfig> nodes;
  std::vector<EdgeConfig> edges;
  std::vector<GraphPortConfig> inputs;
  std::vector<GraphPortConfig> outputs;
  std::vector<SourceSlotConfig> source_slots;
  ConfigValue::Object side_packets;
  FlowControlConfig flow_control;
};

class YamlGraphLoader {
 public:
  static Status LoadFile(const std::string& path, GraphConfig* config);
  static Status LoadString(const std::string& yaml, GraphConfig* config);
  static std::string ExpandEnvironment(const std::string& input);
};

Status MergeSubgraph(GraphConfig* parent, const GraphConfig& child, const std::string& prefix,
                     const ConfigValue::Object& overrides = {});

}  // namespace rkavp
