#include "rkavp/testing/node_test_runner.hpp"

#include <chrono>
#include <utility>

namespace rkavp::testing {

NodeTestRunner::NodeTestRunner(std::unique_ptr<Node> node, std::string node_id,
                               std::string executor)
    : node_(std::move(node)), node_id_(std::move(node_id)), executor_(std::move(executor)) {
  if (node_) contract_ = node_->Contract();
}

NodeTestRunner::~NodeTestRunner() {
  Cancel();
  (void)Close();
}

Status NodeTestRunner::Configure(ConfigValue options) {
  if (!node_) return Status::Invalid("node is required");
  if (options.is_null()) return node_->Configure({});
  if (!options.Is<std::shared_ptr<ConfigValue::Object>>()) {
    return Status::Invalid("node options must be an object");
  }
  return node_->Configure(options.AsObject());
}

Status NodeTestRunner::SetInput(std::string_view port, Packet packet) {
  if (!node_) return Status::Invalid("node is required");
  const std::string name(port);
  if (name.empty() || packet.empty()) return Status::Invalid("input port and packet are required");
  if (contract_.FindInput(name) == nullptr) return Status::NotFound("unknown input port: " + name);
  if (node_->state() == NodeState::kClosed || node_->state() == NodeState::kError) {
    return Status::FailedPrecondition("cannot set input on a closed or failed node");
  }
  inputs_.insert_or_assign(name, std::move(packet));
  return Status::Ok();
}

Status NodeTestRunner::SetSidePacket(std::string_view name, Packet packet) {
  return side_packets_.Set(std::string(name), std::move(packet));
}

void NodeTestRunner::SetResourceManager(std::shared_ptr<ResourceManager> resources) {
  resources_ = std::move(resources);
}

Status NodeTestRunner::Start() {
  if (!node_) return Status::Invalid("node is required");
  if (node_->state() == NodeState::kCreated) {
    Status status = Configure();
    if (!status.ok()) return CleanupAfterFailure(std::move(status));
  }
  if (node_->state() == NodeState::kConfigured) {
    Status status = node_->Open();
    if (!status.ok()) return CleanupAfterFailure(std::move(status));
  }
  cancelled_.store(false);
  start_context_ = MakeContext({});
  Status status = node_->Start(*start_context_);
  if (!status.ok()) return CleanupAfterFailure(std::move(status));
  return status;
}

Status NodeTestRunner::Process() {
  if (!node_) return Status::Invalid("node is required");
  if (node_->state() != NodeState::kRunning) {
    return Status::FailedPrecondition("Process requires a running node");
  }
  PacketSet current_inputs;
  current_inputs.swap(inputs_);
  for (const auto& port : contract_.inputs) {
    if (port.required && current_inputs.find(port.name) == current_inputs.end()) {
      inputs_.swap(current_inputs);
      return Status::Invalid("missing required input: " + port.name);
    }
  }
  auto context = MakeContext(std::move(current_inputs));
  Status status = node_->Process(*context);
  return status.ok() ? status : CleanupAfterFailure(std::move(status));
}

Status NodeTestRunner::Stop() {
  if (!node_) return Status::Invalid("node is required");
  const NodeState current = node_->state();
  if (current == NodeState::kCreated || current == NodeState::kConfigured ||
      current == NodeState::kOpen || current == NodeState::kClosed) {
    return Status::Ok();
  }
  Cancel();
  Status status = node_->Stop();
  start_context_.reset();
  return status;
}

Status NodeTestRunner::Close() {
  if (!node_) return Status::Ok();
  if (node_->state() == NodeState::kRunning) {
    Status status = Stop();
    if (!status.ok()) return status;
  }
  if (node_->state() == NodeState::kCreated || node_->state() == NodeState::kClosed) {
    return Status::Ok();
  }
  Status status = node_->Close();
  start_context_.reset();
  return status;
}

Status NodeTestRunner::RunOnce() {
  Status status = Start();
  if (status.ok()) status = Process();
  Status stop_status = Stop();
  Status close_status = Close();
  if (!status.ok()) return status;
  if (!stop_status.ok()) return stop_status;
  return close_status;
}

std::vector<Packet> NodeTestRunner::Outputs(std::string_view port) const {
  std::lock_guard<std::mutex> lock(output_mutex_);
  const auto it = outputs_.find(std::string(port));
  return it == outputs_.end() ? std::vector<Packet>{} : it->second;
}

std::vector<Packet> NodeTestRunner::TakeOutputs(std::string_view port) {
  std::lock_guard<std::mutex> lock(output_mutex_);
  const auto it = outputs_.find(std::string(port));
  if (it == outputs_.end()) return {};
  std::vector<Packet> packets;
  packets.swap(it->second);
  return packets;
}

Status NodeTestRunner::WaitForOutput(std::string_view port, std::size_t count,
                                     std::int64_t timeout_ms) {
  if (timeout_ms < 0) return Status::Invalid("output wait timeout must be non-negative");
  const std::string name(port);
  if (contract_.FindOutput(name) == nullptr)
    return Status::NotFound("unknown output port: " + name);
  std::unique_lock<std::mutex> lock(output_mutex_);
  const auto ready = [&] {
    const auto it = outputs_.find(name);
    return cancelled_.load() || (it != outputs_.end() && it->second.size() >= count);
  };
  if (!output_condition_.wait_for(lock, std::chrono::milliseconds(timeout_ms), ready)) {
    return Status::Unavailable("timed out waiting for output: " + name);
  }
  if (cancelled_.load()) return Status::Cancelled("output wait was cancelled");
  return Status::Ok();
}

void NodeTestRunner::Cancel() {
  cancelled_.store(true);
  output_condition_.notify_all();
}

NodeState NodeTestRunner::state() const { return node_ ? node_->state() : NodeState::kError; }

Status NodeTestRunner::CaptureOutput(const std::string& port, Packet packet) {
  if (contract_.FindOutput(port) == nullptr)
    return Status::NotFound("unknown output port: " + port);
  {
    std::lock_guard<std::mutex> lock(output_mutex_);
    outputs_[port].push_back(std::move(packet));
  }
  output_condition_.notify_all();
  return Status::Ok();
}

std::unique_ptr<NodeContext> NodeTestRunner::MakeContext(PacketSet inputs) {
  return std::make_unique<NodeContext>(
      node_id_, executor_, std::move(inputs),
      [this](const std::string& port, Packet packet) {
        return CaptureOutput(port, std::move(packet));
      },
      &side_packets_, &services_, resources_.get(), &metrics_, &cancelled_);
}

Status NodeTestRunner::CleanupAfterFailure(Status status) {
  Cancel();
  (void)node_->Stop();
  (void)node_->Close();
  start_context_.reset();
  return status;
}

}  // namespace rkavp::testing
