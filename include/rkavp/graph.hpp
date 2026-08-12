#pragma once

#include <string>
#include <unordered_map>

#include "rkavp/graph_config.hpp"
#include "rkavp/node_registry.hpp"
#include "rkavp/status.hpp"

namespace rkavp {

class Graph {
 public:
  Graph(GraphConfig config, const NodeRegistry* registry)
      : config_(std::move(config)), registry_(registry) {}

  Status Validate() const;
  const GraphConfig& config() const { return config_; }
  std::string Inspect() const;

 private:
  Status ValidateAcyclic(const std::unordered_map<std::string, const NodeConfig*>& nodes) const;
  GraphConfig config_;
  const NodeRegistry* registry_ = nullptr;
};

}  // namespace rkavp
