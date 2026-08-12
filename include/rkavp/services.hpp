#pragma once

#include <memory>
#include <mutex>
#include <string>
#include <typeindex>
#include <unordered_map>

#include "rkavp/packet.hpp"
#include "rkavp/status.hpp"

namespace rkavp {

class SidePacketSet {
 public:
  Status Set(std::string name, Packet packet);
  const Packet* Find(const std::string& name) const;

 private:
  std::unordered_map<std::string, Packet> packets_;
};

class GraphServiceRegistry {
 public:
  template <typename T>
  Status Set(std::string name, std::shared_ptr<T> service) {
    if (name.empty() || !service) return Status::Invalid("service name and instance are required");
    std::lock_guard<std::mutex> lock(mutex_);
    ServiceEntry entry{std::type_index(typeid(T)), std::move(service)};
    if (!services_.emplace(std::move(name), std::move(entry)).second) {
      return Status::AlreadyExists("service is already registered");
    }
    return Status::Ok();
  }

  template <typename T>
  std::shared_ptr<T> Get(const std::string& name) const {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto it = services_.find(name);
    if (it == services_.end() || it->second.type != std::type_index(typeid(T))) return {};
    return std::static_pointer_cast<T>(it->second.value);
  }

  template <typename T>
  Status Replace(std::string name, std::shared_ptr<T> service) {
    if (name.empty() || !service) return Status::Invalid("service name and instance are required");
    std::lock_guard<std::mutex> lock(mutex_);
    services_.insert_or_assign(std::move(name),
                               ServiceEntry{std::type_index(typeid(T)), std::move(service)});
    return Status::Ok();
  }

 private:
  struct ServiceEntry {
    std::type_index type;
    std::shared_ptr<void> value;
  };
  mutable std::mutex mutex_;
  std::unordered_map<std::string, ServiceEntry> services_;
};

class ResourceManager {
 public:
  virtual ~ResourceManager() = default;
  virtual Status Read(const std::string& uri, std::string* data) const = 0;
};

class FileResourceManager final : public ResourceManager {
 public:
  explicit FileResourceManager(std::string root = {});
  Status Read(const std::string& uri, std::string* data) const override;

 private:
  std::string root_;
};

}  // namespace rkavp
