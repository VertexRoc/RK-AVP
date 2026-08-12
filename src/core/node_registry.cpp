#include "rkavp/node_registry.hpp"

#include <algorithm>

namespace rkavp {

Status NodeRegistry::Register(std::string type, Factory factory) {
  if (type.empty() || !factory) {
    return Status::Invalid("node type and factory are required");
  }
  std::lock_guard<std::mutex> lock(mutex_);
  if (factories_.find(type) != factories_.end()) {
    return Status::AlreadyExists("node type is already registered: " + type);
  }
  factories_.emplace(std::move(type), std::move(factory));
  return Status::Ok();
}

Status NodeRegistry::Unregister(const std::string& type) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (factories_.erase(type) == 0) {
    return Status::NotFound("node type is not registered: " + type);
  }
  return Status::Ok();
}

std::unique_ptr<Node> NodeRegistry::Create(const std::string& type) const {
  std::lock_guard<std::mutex> lock(mutex_);
  const auto it = factories_.find(type);
  return it == factories_.end() ? nullptr : it->second();
}

bool NodeRegistry::Contains(const std::string& type) const {
  std::lock_guard<std::mutex> lock(mutex_);
  return factories_.find(type) != factories_.end();
}

std::vector<std::string> NodeRegistry::Types() const {
  std::lock_guard<std::mutex> lock(mutex_);
  std::vector<std::string> types;
  types.reserve(factories_.size());
  for (const auto& item : factories_) {
    types.push_back(item.first);
  }
  std::sort(types.begin(), types.end());
  return types;
}

}  // namespace rkavp
