#include "rkavp/graph.hpp"

#include <functional>
#include <sstream>
#include <unordered_set>

namespace rkavp {

Status Graph::Validate() const {
  if (registry_ == nullptr) return Status::FailedPrecondition("graph has no node registry");
  if (config_.nodes.empty()) return Status::Invalid("graph contains no nodes");
  if (config_.flow_control.observer_queue.capacity == 0) {
    return Status::Invalid("observer queue capacity must be positive");
  }
  if (config_.flow_control.observer_queue.policy == QueuePolicy::kBlock) {
    return Status::Invalid(
        "observer queue policy cannot be block because callbacks must not stall graph executors");
  }

  std::unordered_map<std::string, const ExecutorConfig*> executors;
  for (const auto& executor : config_.executors) {
    if (!executors.emplace(executor.name, &executor).second) {
      return Status::AlreadyExists("duplicate executor: " + executor.name);
    }
  }

  std::unordered_map<std::string, const NodeConfig*> nodes;
  std::unordered_map<std::string, NodeContract> contracts;
  for (const auto& node_config : config_.nodes) {
    if (node_config.id.empty() || node_config.type.empty())
      return Status::Invalid("node id and type are required");
    if (!nodes.emplace(node_config.id, &node_config).second)
      return Status::AlreadyExists("duplicate node id: " + node_config.id);
    if (executors.find(node_config.executor) == executors.end()) {
      return Status::NotFound("node " + node_config.id +
                              " references unknown executor: " + node_config.executor);
    }
    auto node = registry_->Create(node_config.type);
    if (!node) return Status::NotFound("unknown node type: " + node_config.type);
    NodeContract contract = node->Contract();
    const Status options_status = contract.ValidateOptions(node_config.options);
    if (!options_status.ok())
      return Status::Invalid("node " + node_config.id + ": " + options_status.message());
    const InputPolicy policy =
        node_config.has_input_policy ? node_config.input_policy : contract.input_policy;
    const std::string trigger =
        node_config.trigger_port.empty() ? contract.trigger_port : node_config.trigger_port;
    if (policy == InputPolicy::kLatest && !trigger.empty() &&
        contract.FindInput(trigger) == nullptr) {
      return Status::Invalid("node " + node_config.id +
                             " latest trigger port not found: " + trigger);
    }
    contracts.emplace(node_config.id, std::move(contract));
  }

  for (const auto& edge : config_.edges) {
    const auto source = contracts.find(edge.from_node);
    const auto destination = contracts.find(edge.to_node);
    if (source == contracts.end())
      return Status::NotFound("edge source node not found: " + edge.from_node);
    if (destination == contracts.end())
      return Status::NotFound("edge destination node not found: " + edge.to_node);
    const PortSpec* output = source->second.FindOutput(edge.from_port);
    const PortSpec* input = destination->second.FindInput(edge.to_port);
    if (output == nullptr)
      return Status::NotFound("output port not found: " + edge.from_node + "." + edge.from_port);
    if (input == nullptr)
      return Status::NotFound("input port not found: " + edge.to_node + "." + edge.to_port);
    if (!output->caps.CompatibleWith(input->caps)) {
      return Status::Invalid("incompatible media caps on edge " + edge.from_node + "." +
                             edge.from_port + " -> " + edge.to_node + "." + edge.to_port);
    }
    if (edge.back_edge && edge.initial_packet.is_null()) {
      return Status::Invalid("back edge requires initial_packet: " + edge.from_node + "." +
                             edge.from_port);
    }
    const NodeConfig* from = nodes.at(edge.from_node);
    const NodeConfig* to = nodes.at(edge.to_node);
    if (edge.queue.policy == QueuePolicy::kBlock && from->executor == to->executor &&
        executors.at(from->executor)->threads == 1) {
      return Status::Invalid("blocking edge on a single-thread executor can deadlock: " +
                             edge.from_node + " -> " + edge.to_node);
    }
  }

  std::unordered_set<std::string> graph_inputs;
  for (const auto& input : config_.inputs) {
    if (!graph_inputs.insert(input.name).second)
      return Status::AlreadyExists("duplicate graph input: " + input.name);
    if (input.queue.capacity == 0)
      return Status::Invalid("graph input queue capacity must be positive: " + input.name);
    const auto node = contracts.find(input.node);
    if (node == contracts.end() || node->second.FindInput(input.port) == nullptr) {
      return Status::NotFound("graph input endpoint not found: " + input.node + "." + input.port);
    }
  }
  std::unordered_set<std::string> slots;
  for (const auto& slot : config_.source_slots) {
    if (!slots.insert(slot.name).second)
      return Status::AlreadyExists("duplicate source slot: " + slot.name);
    if (graph_inputs.count(slot.input_stream) == 0) {
      return Status::NotFound("source slot references unknown graph input: " + slot.input_stream);
    }
  }

  auto validate_ports = [&](const std::vector<GraphPortConfig>& ports, bool input) -> Status {
    std::unordered_set<std::string> names;
    for (const auto& port : ports) {
      if (!names.insert(port.name).second)
        return Status::AlreadyExists("duplicate graph port: " + port.name);
      const auto node = contracts.find(port.node);
      if (node == contracts.end())
        return Status::NotFound("graph port node not found: " + port.node);
      const PortSpec* spec =
          input ? node->second.FindInput(port.port) : node->second.FindOutput(port.port);
      if (spec == nullptr)
        return Status::NotFound("graph port endpoint not found: " + port.node + "." + port.port);
    }
    return Status::Ok();
  };
  Status status = validate_ports(config_.inputs, true);
  if (!status.ok()) return status;
  status = validate_ports(config_.outputs, false);
  if (!status.ok()) return status;
  return ValidateAcyclic(nodes);
}

Status Graph::ValidateAcyclic(
    const std::unordered_map<std::string, const NodeConfig*>& nodes) const {
  std::unordered_map<std::string, std::vector<std::string>> adjacency;
  for (const auto& edge : config_.edges)
    if (!edge.back_edge) adjacency[edge.from_node].push_back(edge.to_node);
  std::unordered_set<std::string> visiting;
  std::unordered_set<std::string> visited;
  std::function<bool(const std::string&)> visit = [&](const std::string& node) {
    if (visiting.count(node) != 0) return false;
    if (visited.count(node) != 0) return true;
    visiting.insert(node);
    for (const auto& next : adjacency[node])
      if (!visit(next)) return false;
    visiting.erase(node);
    visited.insert(node);
    return true;
  };
  for (const auto& item : nodes)
    if (!visit(item.first))
      return Status::Invalid("graph contains an undeclared cycle involving node: " + item.first);
  return Status::Ok();
}

std::string Graph::Inspect() const {
  std::ostringstream output;
  output << "graph=" << config_.name << " version=" << config_.version << "\n";
  for (const auto& executor : config_.executors) {
    output << "executor " << executor.name << " threads=" << executor.threads
           << " queue_capacity=" << executor.queue_capacity << " priority=" << executor.priority
           << "\n";
  }
  for (const auto& node : config_.nodes)
    output << "node " << node.id << " type=" << node.type << " executor=" << node.executor << "\n";
  for (const auto& edge : config_.edges) {
    output << "edge " << edge.from_node << "." << edge.from_port << " -> " << edge.to_node << "."
           << edge.to_port << " capacity=" << edge.queue.capacity
           << " back_edge=" << (edge.back_edge ? "true" : "false") << "\n";
  }
  return output.str();
}

}  // namespace rkavp
