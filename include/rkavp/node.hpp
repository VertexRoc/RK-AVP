#pragma once

#include <atomic>
#include <functional>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "rkavp/config_value.hpp"
#include "rkavp/media_caps.hpp"
#include "rkavp/metrics.hpp"
#include "rkavp/packet.hpp"
#include "rkavp/services.hpp"
#include "rkavp/status.hpp"

namespace rkavp {

using NodeOptions = ConfigValue::Object;
using PacketSet = std::unordered_map<std::string, Packet>;

struct PortSpec {
  std::string name;
  MediaCaps caps;
  bool required = true;
};

enum class InputPolicy { kAny, kSync, kLatest };

struct NodeContract {
  std::vector<PortSpec> inputs;
  std::vector<PortSpec> outputs;
  std::vector<std::string> required_options;
  InputPolicy input_policy = InputPolicy::kAny;
  std::string trigger_port;

  const PortSpec* FindInput(const std::string& name) const;
  const PortSpec* FindOutput(const std::string& name) const;
  Status ValidateOptions(const NodeOptions& options) const;
};

class NodeContext {
 public:
  using EmitFn = std::function<Status(const std::string&, Packet)>;

  NodeContext(std::string node_id, std::string executor, PacketSet inputs, EmitFn emit,
              const SidePacketSet* side_packets, GraphServiceRegistry* services,
              ResourceManager* resources, MetricsRegistry* metrics,
              const std::atomic<bool>* cancelled);

  const PacketSet& inputs() const { return inputs_; }
  const Packet* Input(const std::string& port) const;
  Status Emit(const std::string& port, Packet packet) const;
  Status SetOutputTimestampBound(const std::string& port, Timestamp bound) const;
  const Packet* SidePacket(const std::string& name) const;

  template <typename T>
  std::shared_ptr<T> Service(const std::string& name) const {
    return services_ == nullptr ? std::shared_ptr<T>{} : services_->Get<T>(name);
  }

  ResourceManager* resources() const { return resources_; }
  MetricsRegistry* metrics() const { return metrics_; }
  bool cancelled() const { return cancelled_ != nullptr && cancelled_->load(); }
  const std::string& node_id() const { return node_id_; }
  const std::string& executor() const { return executor_; }

 private:
  std::string node_id_;
  std::string executor_;
  PacketSet inputs_;
  EmitFn emit_;
  const SidePacketSet* side_packets_ = nullptr;
  GraphServiceRegistry* services_ = nullptr;
  ResourceManager* resources_ = nullptr;
  MetricsRegistry* metrics_ = nullptr;
  const std::atomic<bool>* cancelled_ = nullptr;
};

enum class NodeState { kCreated, kConfigured, kOpen, kRunning, kStopped, kClosed, kError };
enum class ConcurrencyMode { kSerial, kReentrant, kExclusive };

class Node {
 public:
  virtual ~Node() = default;
  virtual NodeContract Contract() const = 0;
  virtual ConcurrencyMode concurrency() const { return ConcurrencyMode::kSerial; }

  Status Configure(const NodeOptions& options);
  Status Open();
  Status Start(NodeContext& context);
  Status Process(NodeContext& context);
  void RequestStop();
  Status Stop();
  Status Close();
  NodeState state() const { return state_.load(); }

 protected:
  virtual Status OnConfigure(const NodeOptions&) { return Status::Ok(); }
  virtual Status OnOpen() { return Status::Ok(); }
  virtual Status OnStart(NodeContext&) { return Status::Ok(); }
  virtual Status OnProcess(NodeContext&) = 0;
  virtual void OnRequestStop() {}
  virtual Status OnStop() { return Status::Ok(); }
  virtual Status OnClose() { return Status::Ok(); }

 private:
  Status Transition(Status status, NodeState success_state);
  std::atomic<NodeState> state_{NodeState::kCreated};
  std::atomic<bool> stop_requested_{false};
};

const ConfigValue* FindOption(const NodeOptions& options, const std::string& key);
Status GetStringOption(const NodeOptions& options, const std::string& key, std::string* value);
Status GetIntegerOption(const NodeOptions& options, const std::string& key, std::int64_t* value);
Status GetBoolOption(const NodeOptions& options, const std::string& key, bool* value);

}  // namespace rkavp
