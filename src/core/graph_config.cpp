#include "rkavp/graph_config.hpp"

#include <yaml-cpp/yaml.h>

#include <cstdlib>
#include <filesystem>
#include <regex>
#include <set>

namespace rkavp {
namespace {

QueuePolicy ParsePolicy(const std::string& value) {
  if (value == "block") return QueuePolicy::kBlock;
  if (value == "drop_newest") return QueuePolicy::kDropNewest;
  return QueuePolicy::kDropOldest;
}

Status ParseQueue(const YAML::Node& value, const QueueConfig& defaults, const std::string& field,
                  QueueConfig* output) {
  *output = defaults;
  if (!value) return Status::Ok();
  if (!value.IsMap()) return Status::Invalid(field + " must be a map");
  output->capacity = value["capacity"] ? value["capacity"].as<std::size_t>() : defaults.capacity;
  const char* default_policy = defaults.policy == QueuePolicy::kBlock        ? "block"
                               : defaults.policy == QueuePolicy::kDropNewest ? "drop_newest"
                                                                             : "drop_oldest";
  output->policy =
      ParsePolicy(value["policy"] ? value["policy"].as<std::string>() : default_policy);
  if (output->capacity == 0) return Status::Invalid(field + " capacity must be positive");
  return Status::Ok();
}

Status ParseInputPolicy(const std::string& value, InputPolicy* policy) {
  if (value == "any")
    *policy = InputPolicy::kAny;
  else if (value == "sync")
    *policy = InputPolicy::kSync;
  else if (value == "latest")
    *policy = InputPolicy::kLatest;
  else
    return Status::Invalid("unknown input policy: " + value);
  return Status::Ok();
}

Status SplitEndpoint(const std::string& endpoint, std::string* node, std::string* port) {
  const std::size_t separator = endpoint.rfind('.');
  if (separator == std::string::npos || separator == 0 || separator + 1 >= endpoint.size()) {
    return Status::Invalid("endpoint must use node.port syntax: " + endpoint);
  }
  *node = endpoint.substr(0, separator);
  *port = endpoint.substr(separator + 1);
  return Status::Ok();
}

ConfigValue ParseValue(const YAML::Node& value) {
  if (!value || value.IsNull()) return {};
  if (value.IsSequence()) {
    ConfigValue::Array array;
    for (const auto& item : value) array.push_back(ParseValue(item));
    return ConfigValue(std::move(array));
  }
  if (value.IsMap()) {
    ConfigValue::Object object;
    for (const auto& item : value)
      object.emplace(item.first.as<std::string>(), ParseValue(item.second));
    return ConfigValue(std::move(object));
  }
  const std::string scalar = YamlGraphLoader::ExpandEnvironment(value.Scalar());
  static const std::regex integer_pattern(R"(^[-+]?[0-9]+$)");
  static const std::regex float_pattern(
      R"(^[-+]?(?:[0-9]+\.[0-9]*|[0-9]*\.[0-9]+)(?:[eE][-+]?[0-9]+)?$)");
  if (scalar == "true") return ConfigValue(true);
  if (scalar == "false") return ConfigValue(false);
  if (std::regex_match(scalar, integer_pattern)) {
    try {
      return ConfigValue(static_cast<std::int64_t>(std::stoll(scalar)));
    } catch (...) {
    }
  }
  if (std::regex_match(scalar, float_pattern)) {
    try {
      return ConfigValue(std::stod(scalar));
    } catch (...) {
    }
  }
  return ConfigValue(scalar);
}

Status ParseGraphPorts(const YAML::Node& ports, std::vector<GraphPortConfig>* output,
                       const std::string& field, bool allow_queue,
                       const QueueConfig& default_queue) {
  if (!ports) return Status::Ok();
  if (!ports.IsMap()) return Status::Invalid(field + " must be a map of name: node.port");
  for (const auto& item : ports) {
    GraphPortConfig port;
    port.queue = default_queue;
    port.name = item.first.as<std::string>();
    const YAML::Node endpoint = item.second.IsMap() ? item.second["to"] : item.second;
    if (!endpoint || !endpoint.IsScalar())
      return Status::Invalid(field + " endpoint must be node.port or {to, queue}");
    Status status = SplitEndpoint(endpoint.as<std::string>(), &port.node, &port.port);
    if (!status.ok()) return status;
    if (allow_queue && item.second.IsMap() && item.second["queue"]) {
      Status status =
          ParseQueue(item.second["queue"], default_queue, field + " queue", &port.queue);
      if (!status.ok()) return status;
    }
    output->push_back(std::move(port));
  }
  return Status::Ok();
}

Status ParseRoot(const YAML::Node& root, const std::filesystem::path& base_path,
                 GraphConfig* config, std::set<std::string>* loading);

Status ParseSubgraphs(const YAML::Node& subgraphs, const std::filesystem::path& base_path,
                      GraphConfig* config, std::set<std::string>* loading,
                      std::unordered_map<std::string, std::string>* input_aliases,
                      std::unordered_map<std::string, std::string>* output_aliases) {
  if (!subgraphs) return Status::Ok();
  if (!subgraphs.IsSequence()) return Status::Invalid("graph.subgraphs must be a sequence");
  for (const auto& item : subgraphs) {
    if (!item["id"] || !item["file"]) return Status::Invalid("subgraph requires id and file");
    const std::string id = item["id"].as<std::string>();
    const std::filesystem::path path = base_path / item["file"].as<std::string>();
    const std::string canonical = std::filesystem::weakly_canonical(path).string();
    if (!loading->insert(canonical).second)
      return Status::Invalid("recursive subgraph include: " + canonical);
    YAML::Node child_root;
    try {
      child_root = YAML::LoadFile(canonical);
    } catch (const YAML::Exception& error) {
      loading->erase(canonical);
      return Status::Invalid(std::string("subgraph YAML error: ") + error.what());
    }
    GraphConfig child;
    Status status = ParseRoot(child_root, path.parent_path(), &child, loading);
    loading->erase(canonical);
    if (!status.ok()) return Status::Invalid("subgraph " + id + ": " + status.message());
    ConfigValue::Object overrides;
    if (item["options"]) {
      ConfigValue value = ParseValue(item["options"]);
      if (!value.Is<std::shared_ptr<ConfigValue::Object>>())
        return Status::Invalid("subgraph options must be a map");
      overrides = value.AsObject();
    }
    status = MergeSubgraph(config, child, id, overrides);
    if (!status.ok()) return status;
    for (const auto& port : child.inputs)
      input_aliases->emplace(id + "." + port.name, id + "." + port.node + "." + port.port);
    for (const auto& port : child.outputs)
      output_aliases->emplace(id + "." + port.name, id + "." + port.node + "." + port.port);
  }
  return Status::Ok();
}

Status ParseRoot(const YAML::Node& root, const std::filesystem::path& base_path,
                 GraphConfig* config, std::set<std::string>* loading) {
  if (!root.IsMap()) return Status::Invalid("YAML root must be a map");
  config->version = root["version"] ? root["version"].as<int>() : 1;
  if (config->version < 1 || config->version > 2) {
    return Status::Invalid("unsupported graph version: " + std::to_string(config->version));
  }
  const YAML::Node graph = root["graph"];
  if (!graph || !graph.IsMap()) return Status::Invalid("missing graph map");
  config->name = graph["name"] ? graph["name"].as<std::string>() : "unnamed";
  if (const YAML::Node flow = graph["flow_control"]) {
    if (config->version < 2) return Status::Invalid("graph.flow_control requires version: 2");
    if (!flow.IsMap()) return Status::Invalid("graph.flow_control must be a map");
    Status status = ParseQueue(flow["input_queue"], config->flow_control.input_queue,
                               "graph.flow_control.input_queue", &config->flow_control.input_queue);
    if (!status.ok()) return status;
    status = ParseQueue(flow["edge_queue"], config->flow_control.edge_queue,
                        "graph.flow_control.edge_queue", &config->flow_control.edge_queue);
    if (!status.ok()) return status;
    status = ParseQueue(flow["observer_queue"], config->flow_control.observer_queue,
                        "graph.flow_control.observer_queue", &config->flow_control.observer_queue);
    if (!status.ok()) return status;
    config->flow_control.observe_timestamp_bounds =
        flow["observe_timestamp_bounds"] ? flow["observe_timestamp_bounds"].as<bool>() : false;
  }
  config->executors.clear();
  if (const YAML::Node executors = graph["executors"]) {
    if (!executors.IsSequence()) return Status::Invalid("graph.executors must be a sequence");
    for (const auto& item : executors) {
      ExecutorConfig executor;
      executor.name = item["name"] ? item["name"].as<std::string>() : "";
      executor.threads = item["threads"] ? item["threads"].as<std::size_t>() : 1;
      executor.queue_capacity =
          item["queue_capacity"] ? item["queue_capacity"].as<std::size_t>() : 256;
      executor.priority = item["priority"] ? item["priority"].as<int>() : 0;
      if (executor.name.empty() || executor.threads == 0 || executor.queue_capacity == 0) {
        return Status::Invalid("executor name, threads and queue_capacity must be valid");
      }
      config->executors.push_back(std::move(executor));
    }
  }
  if (config->executors.empty()) config->executors.push_back({});

  const YAML::Node nodes = graph["nodes"];
  if (!nodes || !nodes.IsSequence()) return Status::Invalid("graph.nodes must be a sequence");
  for (const auto& item : nodes) {
    if (!item["id"] || !item["type"]) return Status::Invalid("each node requires id and type");
    NodeConfig node;
    node.id = item["id"].as<std::string>();
    node.type = item["type"].as<std::string>();
    node.executor = item["executor"] ? item["executor"].as<std::string>() : "default";
    if (item["input_policy"]) {
      node.has_input_policy = true;
      Status status = ParseInputPolicy(item["input_policy"].as<std::string>(), &node.input_policy);
      if (!status.ok()) return Status::Invalid("node " + node.id + ": " + status.message());
    }
    node.trigger_port = item["trigger_port"] ? item["trigger_port"].as<std::string>() : "";
    if (const YAML::Node options = item["options"]) {
      if (!options.IsMap()) return Status::Invalid("node options must be a map: " + node.id);
      for (const auto& option : options)
        node.options.emplace(option.first.as<std::string>(), ParseValue(option.second));
    }
    config->nodes.push_back(std::move(node));
  }

  Status status = ParseGraphPorts(graph["inputs"], &config->inputs, "graph.inputs", true,
                                  config->flow_control.input_queue);
  if (!status.ok()) return status;
  status = ParseGraphPorts(graph["outputs"], &config->outputs, "graph.outputs", false,
                           config->flow_control.observer_queue);
  if (!status.ok()) return status;
  if (const YAML::Node slots = graph["source_slots"]) {
    if (!slots.IsMap()) return Status::Invalid("graph.source_slots must be a map");
    for (const auto& item : slots) {
      SourceSlotConfig slot;
      slot.name = item.first.as<std::string>();
      if (item.second.IsScalar()) {
        slot.input_stream = item.second.as<std::string>();
      } else if (item.second.IsMap()) {
        if (item.second["input"]) slot.input_stream = item.second["input"].as<std::string>();
        if (item.second["template"]) {
          slot.subgraph_template =
              (base_path / item.second["template"].as<std::string>()).lexically_normal().string();
        }
        if (item.second["output"]) slot.output_stream = item.second["output"].as<std::string>();
      } else {
        return Status::Invalid("source slot must be input_stream or {input, template, output}");
      }
      if (slot.name.empty() || slot.input_stream.empty())
        return Status::Invalid("source slot name and input stream are required");
      if (!slot.subgraph_template.empty() && slot.output_stream.empty()) {
        return Status::Invalid("source slot with template requires output stream");
      }
      config->source_slots.push_back(std::move(slot));
    }
  }
  if (graph["side_packets"]) {
    ConfigValue value = ParseValue(graph["side_packets"]);
    if (!value.Is<std::shared_ptr<ConfigValue::Object>>())
      return Status::Invalid("graph.side_packets must be a map");
    config->side_packets = value.AsObject();
  }

  std::unordered_map<std::string, std::string> input_aliases;
  std::unordered_map<std::string, std::string> output_aliases;
  status = ParseSubgraphs(graph["subgraphs"], base_path, config, loading, &input_aliases,
                          &output_aliases);
  if (!status.ok()) return status;

  const YAML::Node edges = graph["edges"];
  if (edges && !edges.IsSequence()) return Status::Invalid("graph.edges must be a sequence");
  if (edges)
    for (const auto& item : edges) {
      if (!item["from"] || !item["to"]) return Status::Invalid("each edge requires from and to");
      std::string from = item["from"].as<std::string>();
      std::string to = item["to"].as<std::string>();
      const auto output_alias = output_aliases.find(from);
      if (output_alias != output_aliases.end()) from = output_alias->second;
      const auto input_alias = input_aliases.find(to);
      if (input_alias != input_aliases.end()) to = input_alias->second;
      EdgeConfig edge;
      edge.queue = config->flow_control.edge_queue;
      status = SplitEndpoint(from, &edge.from_node, &edge.from_port);
      if (!status.ok()) return status;
      status = SplitEndpoint(to, &edge.to_node, &edge.to_port);
      if (!status.ok()) return status;
      if (const YAML::Node queue = item["queue"]) {
        status = ParseQueue(queue, config->flow_control.edge_queue, "edge queue", &edge.queue);
        if (!status.ok()) return status;
      }
      edge.back_edge = item["back_edge"] ? item["back_edge"].as<bool>() : false;
      if (item["initial_packet"]) edge.initial_packet = ParseValue(item["initial_packet"]);
      config->edges.push_back(std::move(edge));
    }
  return Status::Ok();
}

}  // namespace

Status YamlGraphLoader::LoadFile(const std::string& path, GraphConfig* config) {
  if (config == nullptr) return Status::Invalid("config output is null");
  try {
    *config = GraphConfig{};
    std::set<std::string> loading{std::filesystem::weakly_canonical(path).string()};
    return ParseRoot(YAML::LoadFile(path), std::filesystem::path(path).parent_path(), config,
                     &loading);
  } catch (const YAML::Exception& error) {
    return Status::Invalid(std::string("YAML error: ") + error.what());
  }
}

Status YamlGraphLoader::LoadString(const std::string& yaml, GraphConfig* config) {
  if (config == nullptr) return Status::Invalid("config output is null");
  try {
    *config = GraphConfig{};
    std::set<std::string> loading;
    return ParseRoot(YAML::Load(yaml), std::filesystem::current_path(), config, &loading);
  } catch (const YAML::Exception& error) {
    return Status::Invalid(std::string("YAML error: ") + error.what());
  }
}

std::string YamlGraphLoader::ExpandEnvironment(const std::string& input) {
  static const std::regex pattern(R"(\$\{([A-Za-z_][A-Za-z0-9_]*)(:-([^}]*))?\})");
  std::string output;
  std::size_t cursor = 0;
  for (std::sregex_iterator it(input.begin(), input.end(), pattern), end; it != end; ++it) {
    const auto& match = *it;
    output.append(input, cursor, static_cast<std::size_t>(match.position()) - cursor);
    const char* value = std::getenv(match[1].str().c_str());
    if (value != nullptr && *value != '\0')
      output += value;
    else if (match[2].matched)
      output += match[3].str();
    cursor = static_cast<std::size_t>(match.position() + match.length());
  }
  output.append(input, cursor, std::string::npos);
  return output;
}

Status MergeSubgraph(GraphConfig* parent, const GraphConfig& child, const std::string& prefix,
                     const ConfigValue::Object& overrides) {
  if (parent == nullptr || prefix.empty())
    return Status::Invalid("parent graph and prefix are required");
  for (const auto& node : child.nodes) {
    NodeConfig merged = node;
    merged.id = prefix + "." + node.id;
    for (const auto& override_value : overrides) {
      const std::string option_prefix = node.id + ".";
      if (override_value.first.rfind(option_prefix, 0) == 0) {
        merged.options[override_value.first.substr(option_prefix.size())] = override_value.second;
      }
    }
    parent->nodes.push_back(std::move(merged));
  }
  for (const auto& edge : child.edges) {
    EdgeConfig merged = edge;
    merged.from_node = prefix + "." + edge.from_node;
    merged.to_node = prefix + "." + edge.to_node;
    parent->edges.push_back(std::move(merged));
  }
  return Status::Ok();
}

}  // namespace rkavp
