#include "rkavp/node.hpp"

#include <cstdlib>

namespace rkavp {

const PortSpec* NodeContract::FindInput(const std::string& name) const {
  for (const auto& port : inputs)
    if (port.name == name) return &port;
  return nullptr;
}

const PortSpec* NodeContract::FindOutput(const std::string& name) const {
  for (const auto& port : outputs)
    if (port.name == name) return &port;
  return nullptr;
}

Status NodeContract::ValidateOptions(const NodeOptions& options) const {
  for (const auto& name : required_options) {
    if (options.find(name) == options.end())
      return Status::Invalid("missing required option: " + name);
  }
  if (input_policy == InputPolicy::kLatest && !trigger_port.empty() &&
      FindInput(trigger_port) == nullptr) {
    return Status::Invalid("latest input policy trigger port does not exist: " + trigger_port);
  }
  return Status::Ok();
}

NodeContext::NodeContext(std::string node_id, std::string executor, PacketSet inputs, EmitFn emit,
                         const SidePacketSet* side_packets, GraphServiceRegistry* services,
                         ResourceManager* resources, MetricsRegistry* metrics,
                         const std::atomic<bool>* cancelled)
    : node_id_(std::move(node_id)),
      executor_(std::move(executor)),
      inputs_(std::move(inputs)),
      emit_(std::move(emit)),
      side_packets_(side_packets),
      services_(services),
      resources_(resources),
      metrics_(metrics),
      cancelled_(cancelled) {}

const Packet* NodeContext::Input(const std::string& port) const {
  const auto it = inputs_.find(port);
  return it == inputs_.end() ? nullptr : &it->second;
}

Status NodeContext::Emit(const std::string& port, Packet packet) const {
  if (!emit_) return Status::FailedPrecondition("node has no output emitter");
  for (const auto& input : inputs_) packet.InheritLifetimeTokens(input.second);
  return emit_(port, std::move(packet));
}

Status NodeContext::SetOutputTimestampBound(const std::string& port, Timestamp bound) const {
  if (bound.is_unset()) return Status::Invalid("output timestamp bound cannot be unset");
  return Emit(port, Packet::Event(ControlEvent::kTimestampBound, bound));
}

const Packet* NodeContext::SidePacket(const std::string& name) const {
  return side_packets_ == nullptr ? nullptr : side_packets_->Find(name);
}

Status Node::Configure(const NodeOptions& options) {
  if (state_.load() != NodeState::kCreated)
    return Status::FailedPrecondition("Configure requires created state");
  Status status = Contract().ValidateOptions(options);
  if (!status.ok()) return Transition(std::move(status), NodeState::kConfigured);
  return Transition(OnConfigure(options), NodeState::kConfigured);
}

Status Node::Open() {
  if (state_.load() != NodeState::kConfigured)
    return Status::FailedPrecondition("Open requires configured state");
  return Transition(OnOpen(), NodeState::kOpen);
}

Status Node::Start(NodeContext& context) {
  if (state_.load() != NodeState::kOpen && state_.load() != NodeState::kStopped) {
    return Status::FailedPrecondition("Start requires open or stopped state");
  }
  stop_requested_ = false;
  return Transition(OnStart(context), NodeState::kRunning);
}

Status Node::Process(NodeContext& context) {
  if (state_.load() != NodeState::kRunning)
    return Status::FailedPrecondition("Process requires running state");
  return Transition(OnProcess(context), NodeState::kRunning);
}

void Node::RequestStop() {
  if (!stop_requested_.exchange(true)) OnRequestStop();
}

Status Node::Stop() {
  if (state_.load() == NodeState::kStopped || state_.load() == NodeState::kClosed ||
      state_.load() == NodeState::kCreated) {
    return Status::Ok();
  }
  RequestStop();
  return Transition(OnStop(), NodeState::kStopped);
}

Status Node::Close() {
  if (state_.load() == NodeState::kClosed) return Status::Ok();
  if (state_.load() == NodeState::kRunning) {
    Status status = Stop();
    if (!status.ok()) return status;
  }
  return Transition(OnClose(), NodeState::kClosed);
}

Status Node::Transition(Status status, NodeState success_state) {
  state_.store(status.ok() ? success_state : NodeState::kError);
  return status;
}

const ConfigValue* FindOption(const NodeOptions& options, const std::string& key) {
  const auto it = options.find(key);
  return it == options.end() ? nullptr : &it->second;
}

Status GetStringOption(const NodeOptions& options, const std::string& key, std::string* value) {
  if (value == nullptr) return Status::Invalid("string option output is null");
  const ConfigValue* option = FindOption(options, key);
  if (option == nullptr) return Status::NotFound("option not found: " + key);
  if (!option->Is<std::string>()) return Status::Invalid("option must be a string: " + key);
  *value = option->As<std::string>();
  return Status::Ok();
}

Status GetIntegerOption(const NodeOptions& options, const std::string& key, std::int64_t* value) {
  if (value == nullptr) return Status::Invalid("integer option output is null");
  const ConfigValue* option = FindOption(options, key);
  if (option == nullptr) return Status::NotFound("option not found: " + key);
  if (option->Is<std::int64_t>()) {
    *value = option->As<std::int64_t>();
    return Status::Ok();
  }
  return Status::Invalid("option must be an integer: " + key);
}

Status GetBoolOption(const NodeOptions& options, const std::string& key, bool* value) {
  if (value == nullptr) return Status::Invalid("boolean option output is null");
  const ConfigValue* option = FindOption(options, key);
  if (option == nullptr) return Status::NotFound("option not found: " + key);
  if (!option->Is<bool>()) return Status::Invalid("option must be a boolean: " + key);
  *value = option->As<bool>();
  return Status::Ok();
}

}  // namespace rkavp
