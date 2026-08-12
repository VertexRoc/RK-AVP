#pragma once

#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "rkavp/node.hpp"
#include "rkavp/status.hpp"

namespace rkavp {

class NodeRegistry {
 public:
  using Factory = std::function<std::unique_ptr<Node>()>;

  Status Register(std::string type, Factory factory);
  Status Unregister(const std::string& type);
  std::unique_ptr<Node> Create(const std::string& type) const;
  bool Contains(const std::string& type) const;
  std::vector<std::string> Types() const;

 private:
  mutable std::mutex mutex_;
  std::unordered_map<std::string, Factory> factories_;
};

void RegisterBuiltinNodes(NodeRegistry* registry);

}  // namespace rkavp
