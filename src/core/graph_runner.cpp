#include "rkavp/graph_runner.hpp"

#include <algorithm>
#include <chrono>
#include <future>
#include <thread>
#include <utility>

#include "rkavp/logging.hpp"

namespace rkavp {
namespace {

std::int64_t NowMicros() {
  return std::chrono::duration_cast<std::chrono::microseconds>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

std::uint64_t ThreadId() {
  return static_cast<std::uint64_t>(std::hash<std::thread::id>{}(std::this_thread::get_id()));
}

Packet PacketFromConfig(const ConfigValue& value) {
  return Packet::Make(value, Timestamp::FromMicroseconds(0));
}

class ScopeExit {
 public:
  explicit ScopeExit(std::function<void()> action) : action_(std::move(action)) {}
  ~ScopeExit() {
    if (action_) action_();
  }
  ScopeExit(const ScopeExit&) = delete;
  ScopeExit& operator=(const ScopeExit&) = delete;

 private:
  std::function<void()> action_;
};

}  // namespace

GraphRunner::GraphRunner(Graph graph, const NodeRegistry* registry)
    : graph_(std::move(graph)), registry_(registry), trace_(4096) {
  services_.Set("rkavp.hardware", hardware_context_);
  services_.Set("rkavp.clock", clock_);
}

GraphRunner::~GraphRunner() {
  Stop();
  ShutdownObservers();
  ShutdownErrorCallback();
}

Status GraphRunner::SetSidePacket(std::string name, Packet packet) {
  if (running_) return Status::FailedPrecondition("side packets must be set before Start");
  return side_packets_.Set(std::move(name), std::move(packet));
}

void GraphRunner::SetResourceManager(std::shared_ptr<ResourceManager> resources) {
  if (!running_ && resources) resources_ = std::move(resources);
}

Status GraphRunner::ObserveOutput(const std::string& stream, OutputCallback callback) {
  ObserverOptions options;
  options.queue_capacity = graph_.config().flow_control.observer_queue.capacity;
  options.queue_policy = graph_.config().flow_control.observer_queue.policy;
  options.observe_timestamp_bounds = graph_.config().flow_control.observe_timestamp_bounds;
  ObserverHandle handle;
  return ObserveOutput(stream, std::move(callback), options, &handle);
}

Status GraphRunner::ObserveOutput(const std::string& stream, OutputCallback callback,
                                  ObserverOptions options, ObserverHandle* handle) {
  if (!callback) return Status::Invalid("output callback is empty");
  if (handle == nullptr) return Status::Invalid("observer handle output is null");
  if (options.queue_capacity == 0)
    return Status::Invalid("observer queue capacity must be positive");
  if (options.queue_policy == QueuePolicy::kBlock) {
    return Status::Invalid(
        "observer queue policy cannot be block because callbacks must not stall graph executors");
  }
  bool found = false;
  for (const auto& output : graph_.config().outputs)
    if (output.name == stream) found = true;
  if (!found) return Status::NotFound("graph output stream not found: " + stream);
  auto observer = std::make_shared<ObserverRuntime>();
  observer->handle.id = next_observer_id_++;
  observer->stream = stream;
  observer->callback = std::move(callback);
  observer->options = options;
  observer->worker = std::thread([this, observer] { RunObserver(observer); });
  std::lock_guard<std::mutex> lock(state_mutex_);
  observers_[stream].push_back(observer);
  observer_handles_[observer->handle.id] = observer;
  *handle = observer->handle;
  return Status::Ok();
}

Status GraphRunner::CancelObserver(ObserverHandle handle) {
  std::shared_ptr<ObserverRuntime> observer;
  {
    std::lock_guard<std::mutex> lock(state_mutex_);
    const auto it = observer_handles_.find(handle.id);
    if (it == observer_handles_.end()) return Status::NotFound("observer handle not found");
    observer = it->second;
    observer_handles_.erase(it);
    auto& stream_observers = observers_[observer->stream];
    stream_observers.erase(std::remove(stream_observers.begin(), stream_observers.end(), observer),
                           stream_observers.end());
  }
  {
    std::lock_guard<std::mutex> lock(observer->mutex);
    observer->stopped = true;
    observer->condition.notify_all();
  }
  if (observer->worker.joinable() && observer->worker.get_id() != std::this_thread::get_id()) {
    observer->worker.join();
  } else if (observer->worker.joinable()) {
    std::lock_guard<std::mutex> lock(state_mutex_);
    retired_observers_.push_back(observer);
  }
  return Status::Ok();
}

Status GraphRunner::WaitForObservedOutput(ObserverHandle handle, std::int64_t timeout_ms) {
  std::shared_ptr<ObserverRuntime> observer;
  {
    std::lock_guard<std::mutex> lock(state_mutex_);
    const auto it = observer_handles_.find(handle.id);
    if (it == observer_handles_.end()) return Status::NotFound("observer handle not found");
    observer = it->second;
  }
  std::unique_lock<std::mutex> lock(observer->mutex);
  const auto ready = [&] { return observer->stopped || observer->delivered > observer->waited; };
  if (timeout_ms < 0)
    observer->condition.wait(lock, ready);
  else if (!observer->condition.wait_for(lock, std::chrono::milliseconds(timeout_ms), ready)) {
    return Status::Unavailable("wait for observed output timed out");
  }
  if (observer->delivered > observer->waited) {
    observer->waited = observer->delivered;
    return Status::Ok();
  }
  return Status::Cancelled("observer was cancelled");
}

Status GraphRunner::SetErrorCallback(ErrorCallback callback) {
  if (running_) return Status::FailedPrecondition("error callback must be set before Start");
  ShutdownErrorCallback();
  if (!callback) return Status::Ok();
  auto runtime = std::make_shared<ErrorCallbackRuntime>();
  runtime->callback = std::move(callback);
  runtime->worker = std::thread([runtime] {
    for (;;) {
      Status status;
      {
        std::unique_lock<std::mutex> lock(runtime->mutex);
        runtime->condition.wait(lock, [&] { return runtime->stopped || !runtime->queue.empty(); });
        if (runtime->stopped && runtime->queue.empty()) return;
        status = std::move(runtime->queue.front());
        runtime->queue.pop_front();
      }
      try {
        runtime->callback(status);
      } catch (...) {
      }
    }
  });
  std::lock_guard<std::mutex> lock(error_mutex_);
  error_callback_runtime_ = std::move(runtime);
  return Status::Ok();
}

void GraphRunner::SetClock(std::shared_ptr<IClock> clock) {
  if (!running_ && clock) {
    clock_ = std::move(clock);
    services_.Replace("rkavp.clock", clock_);
  }
}

Status GraphRunner::Start() {
  std::lock_guard<std::recursive_mutex> transition_lock(transition_mutex_);
  {
    std::lock_guard<std::mutex> lock(lifecycle_mutex_);
    if (running_) return Status::AlreadyExists("graph runner is already running");
    if (stopping_) return Status::FailedPrecondition("graph runner is stopping");
  }
  Status status = graph_.Validate();
  if (!status.ok()) return status;
  cancelled_ = false;
  {
    std::lock_guard<std::mutex> lock(error_mutex_);
    last_error_ = Status::Ok();
  }

  for (const auto& config : graph_.config().executors) {
    auto scheduler = std::make_unique<Scheduler>(config.threads, config.queue_capacity, config.name,
                                                 config.priority, clock_);
    status = scheduler->Start();
    if (!status.ok()) {
      Stop();
      return status;
    }
    executors_.emplace(config.name, std::move(scheduler));
  }
  control_executor_ = std::make_unique<Scheduler>(1, 64, "control", 0, clock_);
  status = control_executor_->Start();
  if (!status.ok()) {
    Stop();
    return status;
  }

  for (const auto& config : graph_.config().nodes) {
    auto runtime = std::make_unique<RuntimeNode>();
    runtime->config = config;
    runtime->node = registry_->Create(config.type);
    if (!runtime->node) {
      Stop();
      return Status::NotFound("node type disappeared: " + config.type);
    }
    runtime->contract = runtime->node->Contract();
    if (config.has_input_policy) runtime->contract.input_policy = config.input_policy;
    if (!config.trigger_port.empty()) runtime->contract.trigger_port = config.trigger_port;
    status = runtime->node->Configure(config.options);
    if (status.ok()) status = runtime->node->Open();
    if (!status.ok()) {
      runtime->node->Close();
      Stop();
      return Status::Internal("node " + config.id + ": " + status.message());
    }
    nodes_.emplace(config.id, std::move(runtime));
    lifecycle_order_.push_back(config.id);
    if (nodes_.at(config.id)->contract.inputs.empty()) open_source_nodes_.insert(config.id);
  }

  for (const auto& edge : graph_.config().edges) {
    auto runtime = std::make_unique<EdgeRuntime>();
    runtime->config = edge;
    runtime->queue = std::make_unique<BoundedQueue<Packet>>(edge.queue.capacity, edge.queue.policy);
    const std::size_t index = edges_.size();
    routes_[edge.from_node + "." + edge.from_port].push_back(index);
    edges_.push_back(std::move(runtime));
  }
  for (const auto& input : graph_.config().inputs) {
    auto runtime = std::make_unique<InputRuntime>();
    runtime->config = input;
    runtime->queue =
        std::make_unique<BoundedQueue<Packet>>(input.queue.capacity, input.queue.policy);
    graph_inputs_.emplace(input.name, std::move(runtime));
    open_inputs_.insert(input.name);
  }
  source_slots_.clear();
  for (const auto& slot : graph_.config().source_slots) source_slots_[slot.name] = slot;
  for (const auto& output : graph_.config().outputs) {
    graph_outputs_[output.node + "." + output.port].push_back(output.name);
  }
  for (const auto& side : graph_.config().side_packets) {
    if (side_packets_.Find(side.first) == nullptr) {
      status = side_packets_.Set(side.first, PacketFromConfig(side.second));
      if (!status.ok()) {
        Stop();
        return status;
      }
    }
  }

  running_ = true;
  for (const auto& node_id : lifecycle_order_) {
    auto& runtime = nodes_.at(node_id);
    runtime->start_context = std::make_unique<NodeContext>(
        node_id, runtime->config.executor, PacketSet{},
        [this, node_id](const std::string& port, const Packet& packet) {
          return Emit(node_id, port, packet);
        },
        &side_packets_, &services_, resources_.get(), &metrics_, &cancelled_);
    status = runtime->node->Start(*runtime->start_context);
    if (!status.ok()) {
      SetError(Status::Internal("node " + node_id + " start failed: " + status.message()));
      Stop();
      return status;
    }
  }
  for (std::size_t i = 0; i < edges_.size(); ++i) {
    if (edges_[i]->config.back_edge)
      EnqueueEdge(i, PacketFromConfig(edges_[i]->config.initial_packet));
  }
  ScopedLogContext log_context({graph_.config().name, "", "", std::nullopt, "", "", std::nullopt});
  RKAVP_LOG(Info) << "graph started with " << nodes_.size() << " nodes and " << executors_.size()
                  << " executors";
  return Status::Ok();
}

Status GraphRunner::AddPacket(const std::string& stream, Packet packet) {
  if (!BeginOperation()) return Status::FailedPrecondition("graph runner is not accepting input");
  ScopeExit operation([this] { EndOperation(); });
  if (cancelled_) return Status::Cancelled("graph runner is cancelled");
  const auto input = graph_inputs_.find(stream);
  if (input == graph_inputs_.end())
    return Status::NotFound("graph input stream not found: " + stream);
  {
    std::lock_guard<std::mutex> lock(input->second->state_mutex);
    Status status = ValidateStreamPacket(packet.timestamp(), &input->second->last_timestamp,
                                         input->second->bound, !input->second->open, stream);
    if (!status.ok()) return status;
  }
  bool dropped = false;
  const bool pushed = input->second->queue->Push(std::move(packet), &dropped);
  if (dropped) {
    ++input->second->dropped;
    metrics_.Increment("input." + stream + ".dropped");
  }
  if (!pushed) {
    if (dropped && input->second->config.queue.policy == QueuePolicy::kDropNewest)
      return Status::Ok();
    return cancelled_ ? Status::Cancelled("graph input queue is closed")
                      : Status::Unavailable("graph input queue rejected packet: " + stream);
  }
  ScheduleInput(stream);
  return Status::Ok();
}

Status GraphRunner::SetInputTimestampBound(const std::string& stream, Timestamp bound) {
  if (!BeginOperation()) return Status::FailedPrecondition("graph runner is not accepting input");
  ScopeExit operation([this] { EndOperation(); });
  if (bound.is_unset()) return Status::Invalid("timestamp bound cannot be unset");
  const auto input = graph_inputs_.find(stream);
  if (input == graph_inputs_.end())
    return Status::NotFound("graph input stream not found: " + stream);
  const auto node = nodes_.find(input->second->config.node);
  if (node == nodes_.end()) return Status::NotFound("runtime node not found");
  {
    std::lock_guard<std::mutex> lock(input->second->state_mutex);
    if (!input->second->bound.is_unset() && bound < input->second->bound) {
      return Status::Invalid("timestamp bound cannot move backwards");
    }
    input->second->bound = bound;
  }
  std::lock_guard<std::mutex> lock(node->second->input_mutex);
  const auto old = node->second->bounds.find(input->second->config.port);
  if (old != node->second->bounds.end() && bound < old->second)
    return Status::Invalid("timestamp bound cannot move backwards");
  node->second->bounds[input->second->config.port] = bound;
  PruneImpossibleSyncPacketsLocked(node->second.get());
  return Status::Ok();
}

Status GraphRunner::CloseInputStream(const std::string& stream) {
  if (!BeginOperation()) return Status::FailedPrecondition("graph runner is not accepting input");
  ScopeExit operation([this] { EndOperation(); });
  const auto input = graph_inputs_.find(stream);
  if (input == graph_inputs_.end())
    return Status::NotFound("graph input stream not found: " + stream);
  {
    std::lock_guard<std::mutex> lock(state_mutex_);
    if (open_inputs_.erase(stream) == 0)
      return Status::FailedPrecondition("graph input stream is already closed: " + stream);
  }
  {
    std::lock_guard<std::mutex> lock(input->second->state_mutex);
    input->second->open = false;
  }
  bool dropped = false;
  if (!input->second->queue->PushControl(
          Packet::Event(ControlEvent::kEndOfStream, Timestamp::Done()), &dropped)) {
    return Status::Unavailable("failed to enqueue input EOS: " + stream);
  }
  ScheduleInput(stream);
  state_condition_.notify_all();
  return Status::Ok();
}

Status GraphRunner::WaitUntilIdle(std::int64_t timeout_ms) {
  if (!BeginOperation()) return Status::FailedPrecondition("graph runner is not running");
  ScopeExit operation([this] { EndOperation(); });
  const auto start = std::chrono::steady_clock::now();
  for (;;) {
    bool idle = true;
    for (const auto& input : graph_inputs_) {
      if (input.second->queue->size() != 0 || input.second->scheduled) idle = false;
    }
    for (const auto& edge : edges_)
      if (edge->queue->size() != 0 || edge->scheduled) idle = false;
    for (const auto& executor : executors_)
      if (executor.second->queued() != 0 || executor.second->active() != 0) idle = false;
    std::vector<std::shared_ptr<ObserverRuntime>> observers;
    {
      std::lock_guard<std::mutex> lock(state_mutex_);
      for (const auto& item : observer_handles_) observers.push_back(item.second);
    }
    for (const auto& observer : observers) {
      std::lock_guard<std::mutex> observer_lock(observer->mutex);
      if (!observer->queue.empty() || observer->active) idle = false;
    }
    if (idle) return last_error();
    if (timeout_ms >= 0 && std::chrono::duration_cast<std::chrono::milliseconds>(
                               std::chrono::steady_clock::now() - start)
                                   .count() >= timeout_ms) {
      return Status(StatusCode::kUnavailable, "graph wait idle timed out");
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
}

Status GraphRunner::WaitUntilDone(std::int64_t timeout_ms) {
  if (!BeginOperation()) return Status::FailedPrecondition("graph runner is not running");
  ScopeExit operation([this] { EndOperation(); });
  const auto start = std::chrono::steady_clock::now();
  {
    std::unique_lock<std::mutex> lock(state_mutex_);
    const auto done = [this] {
      return (open_inputs_.empty() && open_source_nodes_.empty() && active_sources_ == 0) ||
             cancelled_;
    };
    if (timeout_ms < 0)
      state_condition_.wait(lock, done);
    else if (!state_condition_.wait_for(lock, std::chrono::milliseconds(timeout_ms), done)) {
      return Status(StatusCode::kUnavailable, "graph wait done timed out waiting for input close");
    }
  }
  if (cancelled_) {
    Status error = last_error();
    return error.ok() ? Status::Cancelled("graph was cancelled") : error;
  }
  const std::int64_t elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                                   std::chrono::steady_clock::now() - start)
                                   .count();
  return WaitUntilIdle(timeout_ms < 0 ? -1 : std::max<std::int64_t>(0, timeout_ms - elapsed));
}

void GraphRunner::Cancel() {
  cancelled_ = true;
  CloseQueues();
  state_condition_.notify_all();
}

void GraphRunner::Stop() {
  std::lock_guard<std::recursive_mutex> transition_lock(transition_mutex_);
  {
    std::unique_lock<std::mutex> lock(lifecycle_mutex_);
    if (stopping_) {
      lifecycle_condition_.wait(lock, [this] { return !stopping_; });
      return;
    }
    if (!running_.exchange(false) && nodes_.empty() && executors_.empty()) return;
    stopping_ = true;
  }
  cancelled_ = true;
  std::unordered_map<std::string, SourceRuntime> sources;
  {
    std::lock_guard<std::mutex> lock(source_mutex_);
    sources.swap(sources_);
  }
  for (auto& item : nodes_) item.second->node->RequestStop();
  for (auto& source : sources)
    if (source.second.subgraph) source.second.subgraph->Stop();
  CloseQueues();
  if (control_executor_) control_executor_->Stop(false);
  for (auto& executor : executors_) executor.second->Stop(true);
  {
    std::unique_lock<std::mutex> lock(lifecycle_mutex_);
    lifecycle_condition_.wait(lock, [this] { return active_operations_ == 0; });
  }
  for (auto it = lifecycle_order_.rbegin(); it != lifecycle_order_.rend(); ++it) {
    const auto node = nodes_.find(*it);
    if (node != nodes_.end()) node->second->node->Stop();
  }
  for (auto it = lifecycle_order_.rbegin(); it != lifecycle_order_.rend(); ++it) {
    const auto node = nodes_.find(*it);
    if (node != nodes_.end()) node->second->node->Close();
  }
  nodes_.clear();
  lifecycle_order_.clear();
  edges_.clear();
  routes_.clear();
  graph_inputs_.clear();
  graph_outputs_.clear();
  open_inputs_.clear();
  open_source_nodes_.clear();
  active_sources_ = 0;
  executors_.clear();
  control_executor_.reset();
  {
    std::lock_guard<std::mutex> lock(lifecycle_mutex_);
    stopping_ = false;
  }
  lifecycle_condition_.notify_all();
  state_condition_.notify_all();
}

Status GraphRunner::last_error() const {
  std::lock_guard<std::mutex> lock(error_mutex_);
  return last_error_;
}

Status GraphRunner::Emit(const std::string& node_id, const std::string& output_port,
                         const Packet& packet) {
  if (cancelled_) return Status::Cancelled("graph is cancelled");
  if (packet.event() == ControlEvent::kEndOfStream) {
    std::lock_guard<std::mutex> lock(state_mutex_);
    if (open_source_nodes_.erase(node_id) != 0) state_condition_.notify_all();
  }
  const std::string endpoint = node_id + "." + output_port;
  const auto output = graph_outputs_.find(endpoint);
  if (output != graph_outputs_.end()) {
    std::vector<std::shared_ptr<ObserverRuntime>> callbacks;
    {
      std::lock_guard<std::mutex> lock(state_mutex_);
      for (const auto& stream : output->second) {
        const auto observer = observers_.find(stream);
        if (observer != observers_.end())
          callbacks.insert(callbacks.end(), observer->second.begin(), observer->second.end());
      }
    }
    for (const auto& observer : callbacks) {
      if (packet.event() != ControlEvent::kTimestampBound ||
          observer->options.observe_timestamp_bounds) {
        EnqueueObserver(observer, packet);
      }
    }
  }
  const auto route = routes_.find(endpoint);
  if (route == routes_.end()) return Status::Ok();
  Status first_error = Status::Ok();
  for (std::size_t edge : route->second) {
    Status status = EnqueueEdge(edge, packet);
    if (!status.ok() && first_error.ok()) first_error = status;
  }
  return first_error;
}

Status GraphRunner::EnqueueEdge(std::size_t edge_index, Packet packet) {
  if (edge_index >= edges_.size()) return Status::Internal("edge index out of range");
  EdgeRuntime* edge = edges_[edge_index].get();
  {
    std::lock_guard<std::mutex> lock(edge->state_mutex);
    if (packet.event() == ControlEvent::kTimestampBound) {
      if (!edge->bound.is_unset() && packet.timestamp() < edge->bound) {
        return Status::Invalid("edge timestamp bound cannot move backwards");
      }
      edge->bound = packet.timestamp();
    } else if (packet.event() == ControlEvent::kEndOfStream) {
      edge->closed = true;
    } else {
      Status status =
          ValidateStreamPacket(packet.timestamp(), &edge->last_timestamp, edge->bound, edge->closed,
                               edge->config.from_node + "." + edge->config.from_port);
      if (!status.ok()) return status;
    }
  }
  bool dropped = false;
  const bool control = packet.event() == ControlEvent::kTimestampBound ||
                       packet.event() == ControlEvent::kEndOfStream;
  const bool pushed = control ? edge->queue->PushControl(std::move(packet), &dropped)
                              : edge->queue->Push(std::move(packet), &dropped);
  const std::string metric = "edge." + edge->config.from_node + "." + edge->config.from_port +
                             "_to_" + edge->config.to_node + "." + edge->config.to_port;
  if (dropped) {
    ++edge->dropped;
    metrics_.Increment(metric + ".dropped");
  }
  metrics_.SetGauge(metric + ".depth", static_cast<double>(edge->queue->size()));
  if (!pushed) {
    if (dropped && edge->config.queue.policy == QueuePolicy::kDropNewest) return Status::Ok();
    return cancelled_ ? Status::Cancelled("edge is closed")
                      : Status(StatusCode::kUnavailable, "edge rejected packet");
  }
  ScheduleEdge(edge_index);
  return Status::Ok();
}

void GraphRunner::ScheduleEdge(std::size_t edge_index) {
  EdgeRuntime* edge = edges_[edge_index].get();
  if (edge->scheduled.exchange(true)) return;
  Scheduler* executor = ExecutorFor(edge->config.to_node);
  Status status = executor == nullptr
                      ? Status::NotFound("destination executor not found")
                      : executor->PostGuaranteed([this, edge_index] { DrainEdge(edge_index); });
  if (!status.ok()) {
    edge->scheduled = false;
    SetError(status);
  }
}

void GraphRunner::DrainEdge(std::size_t edge_index) {
  if (edge_index >= edges_.size()) return;
  EdgeRuntime* edge = edges_[edge_index].get();
  Packet packet;
  if (edge->queue->TryPop(&packet)) {
    metrics_.Increment("edge.delivered");
    Status status = Deliver(edge->config.to_node, edge->config.to_port, std::move(packet));
    if (!status.ok()) SetError(status);
  }
  edge->scheduled = false;
  if (!cancelled_ && edge->queue->size() != 0) ScheduleEdge(edge_index);
}

void GraphRunner::ScheduleInput(const std::string& stream) {
  const auto input = graph_inputs_.find(stream);
  if (input == graph_inputs_.end() || input->second->scheduled.exchange(true)) return;
  Scheduler* executor = ExecutorFor(input->second->config.node);
  Status status = executor == nullptr
                      ? Status::NotFound("input destination executor not found")
                      : executor->PostGuaranteed([this, stream] { DrainInput(stream); });
  if (!status.ok()) {
    input->second->scheduled = false;
    SetError(status);
  }
}

void GraphRunner::DrainInput(const std::string& stream) {
  const auto input = graph_inputs_.find(stream);
  if (input == graph_inputs_.end()) return;
  Packet packet;
  if (input->second->queue->TryPop(&packet)) {
    Status status =
        Deliver(input->second->config.node, input->second->config.port, std::move(packet));
    if (!status.ok()) SetError(status);
  }
  input->second->scheduled = false;
  if (!cancelled_ && input->second->queue->size() != 0) ScheduleInput(stream);
}

Status GraphRunner::ValidateStreamPacket(Timestamp timestamp, Timestamp* last_timestamp,
                                         Timestamp bound, bool closed,
                                         const std::string& stream) const {
  if (closed) return Status::FailedPrecondition("stream is closed: " + stream);
  if (!timestamp.is_range_value())
    return Status::Invalid("data packet requires a range timestamp: " + stream);
  if (!bound.is_unset() && timestamp < bound) {
    return Status::Invalid("packet timestamp is below stream bound: " + stream);
  }
  if (last_timestamp != nullptr && last_timestamp->is_range_value() &&
      timestamp < *last_timestamp) {
    return Status::Invalid("packet timestamp moved backwards: " + stream);
  }
  if (last_timestamp != nullptr) *last_timestamp = timestamp;
  return Status::Ok();
}

void GraphRunner::EnqueueObserver(const std::shared_ptr<ObserverRuntime>& observer,
                                  const Packet& packet) {
  std::unique_lock<std::mutex> lock(observer->mutex);
  if (observer->stopped) return;
  if (observer->queue.size() >= observer->options.queue_capacity) {
    ++observer->dropped;
    metrics_.Increment("observer." + observer->stream + ".dropped");
    if (observer->options.queue_policy == QueuePolicy::kDropNewest) return;
    observer->queue.pop_front();
  }
  observer->queue.push_back(packet);
  observer->condition.notify_all();
}

void GraphRunner::RunObserver(const std::shared_ptr<ObserverRuntime>& observer) {
  for (;;) {
    Packet packet;
    {
      std::unique_lock<std::mutex> lock(observer->mutex);
      observer->condition.wait(lock, [&] { return observer->stopped || !observer->queue.empty(); });
      if (observer->stopped && observer->queue.empty()) return;
      packet = std::move(observer->queue.front());
      observer->queue.pop_front();
      observer->active = true;
      observer->condition.notify_all();
    }
    try {
      observer->callback(packet);
    } catch (...) {
      metrics_.Increment("graph.output_callback_errors");
    }
    {
      std::lock_guard<std::mutex> lock(observer->mutex);
      observer->active = false;
      ++observer->delivered;
      observer->condition.notify_all();
    }
  }
}

void GraphRunner::ShutdownObservers() {
  std::vector<std::shared_ptr<ObserverRuntime>> observers;
  {
    std::lock_guard<std::mutex> lock(state_mutex_);
    for (const auto& item : observer_handles_) observers.push_back(item.second);
    observers.insert(observers.end(), retired_observers_.begin(), retired_observers_.end());
    observer_handles_.clear();
    observers_.clear();
    retired_observers_.clear();
  }
  for (const auto& observer : observers) {
    {
      std::lock_guard<std::mutex> lock(observer->mutex);
      observer->stopped = true;
      observer->condition.notify_all();
    }
    if (observer->worker.joinable()) observer->worker.join();
  }
}

void GraphRunner::ShutdownErrorCallback() {
  std::shared_ptr<ErrorCallbackRuntime> runtime;
  {
    std::lock_guard<std::mutex> lock(error_mutex_);
    runtime = std::move(error_callback_runtime_);
  }
  if (!runtime) return;
  {
    std::lock_guard<std::mutex> lock(runtime->mutex);
    runtime->stopped = true;
    runtime->condition.notify_all();
  }
  if (!runtime->worker.joinable()) return;
  if (runtime->worker.get_id() == std::this_thread::get_id())
    runtime->worker.detach();
  else
    runtime->worker.join();
}

Status GraphRunner::Deliver(const std::string& node_id, const std::string& port, Packet packet) {
  const auto node = nodes_.find(node_id);
  if (node == nodes_.end()) return Status::NotFound("runtime node not found: " + node_id);
  if (packet.event() == ControlEvent::kTimestampBound) {
    std::lock_guard<std::mutex> lock(node->second->input_mutex);
    const auto old = node->second->bounds.find(port);
    if (old != node->second->bounds.end() && packet.timestamp() < old->second) {
      return Status::Invalid("timestamp bound cannot move backwards on " + node_id + "." + port);
    }
    node->second->bounds[port] = packet.timestamp();
    PruneImpossibleSyncPacketsLocked(node->second.get());
    return Status::Ok();
  }
  PacketSet inputs;
  if (!PrepareInvocation(node->second.get(), port, std::move(packet), &inputs)) return Status::Ok();
  Invoke(node_id, std::move(inputs));
  return Status::Ok();
}

bool GraphRunner::PrepareInvocation(RuntimeNode* runtime, const std::string& port, Packet packet,
                                    PacketSet* inputs) {
  std::lock_guard<std::mutex> lock(runtime->input_mutex);
  const InputPolicy policy = runtime->contract.input_policy;
  if (policy == InputPolicy::kAny) {
    inputs->emplace(port, std::move(packet));
    return true;
  }
  if (packet.event() == ControlEvent::kEndOfStream) runtime->closed_ports.insert(port);
  if (policy == InputPolicy::kLatest) {
    runtime->latest[port] = std::move(packet);
    std::string trigger = runtime->contract.trigger_port;
    if (trigger.empty() && !runtime->contract.inputs.empty())
      trigger = runtime->contract.inputs.front().name;
    if (port != trigger) return false;
    for (const auto& spec : runtime->contract.inputs) {
      const auto latest = runtime->latest.find(spec.name);
      if (spec.required && latest == runtime->latest.end()) return false;
      if (latest != runtime->latest.end()) inputs->emplace(spec.name, latest->second);
    }
    return true;
  }

  const Timestamp timestamp = packet.timestamp();
  if (!timestamp.is_range_value() && packet.event() != ControlEvent::kEndOfStream) {
    SetError(Status::Invalid("sync input requires a range timestamp on " + runtime->config.id +
                             "." + port));
    return false;
  }
  if (packet.event() != ControlEvent::kEndOfStream)
    runtime->pending[port][timestamp.microseconds()] = std::move(packet);
  PruneImpossibleSyncPacketsLocked(runtime);
  for (const auto& candidate : runtime->pending[port]) {
    const std::int64_t ts = candidate.first;
    bool ready = true;
    for (const auto& spec : runtime->contract.inputs) {
      if (spec.required && runtime->pending[spec.name].count(ts) == 0) {
        ready = false;
        break;
      }
    }
    if (!ready) continue;
    for (const auto& spec : runtime->contract.inputs) {
      auto match = runtime->pending[spec.name].find(ts);
      if (match != runtime->pending[spec.name].end()) {
        inputs->emplace(spec.name, std::move(match->second));
        runtime->pending[spec.name].erase(match);
      }
    }
    return true;
  }
  bool all_closed = true;
  for (const auto& spec : runtime->contract.inputs) {
    if (spec.required && runtime->closed_ports.count(spec.name) == 0) all_closed = false;
  }
  if (all_closed && !runtime->eos_dispatched) {
    runtime->eos_dispatched = true;
    for (const auto& spec : runtime->contract.inputs)
      if (spec.required) {
        inputs->emplace(spec.name, Packet::Event(ControlEvent::kEndOfStream, Timestamp::Done()));
      }
    return true;
  }
  return false;
}

void GraphRunner::PruneImpossibleSyncPacketsLocked(RuntimeNode* runtime) {
  if (runtime->contract.input_policy != InputPolicy::kSync) return;
  std::unordered_set<std::int64_t> candidates;
  for (const auto& port : runtime->pending) {
    for (const auto& packet : port.second) candidates.insert(packet.first);
  }
  for (const std::int64_t timestamp : candidates) {
    bool impossible = false;
    for (const auto& spec : runtime->contract.inputs) {
      if (!spec.required || runtime->pending[spec.name].count(timestamp) != 0) continue;
      const auto bound = runtime->bounds.find(spec.name);
      if (runtime->closed_ports.count(spec.name) != 0 ||
          (bound != runtime->bounds.end() &&
           (bound->second.is_done() ||
            (bound->second.is_range_value() && bound->second.microseconds() > timestamp)))) {
        impossible = true;
        break;
      }
    }
    if (!impossible) continue;
    for (auto& port : runtime->pending) port.second.erase(timestamp);
    metrics_.Increment("graph.sync_unmatched_dropped");
  }
}

void GraphRunner::Invoke(const std::string& node_id, PacketSet inputs) {
  const auto node = nodes_.find(node_id);
  if (node == nodes_.end() || cancelled_) return;
  RuntimeNode* runtime = node->second.get();
  const std::int64_t start = NowMicros();
  std::optional<std::int64_t> pts;
  if (!inputs.empty() && inputs.begin()->second.timestamp().is_range_value()) {
    pts = inputs.begin()->second.timestamp().microseconds();
  }
  ScopedLogContext log_context(
      {graph_.config().name, node_id, "", std::nullopt, runtime->config.executor, "", pts});
  NodeContext context(
      node_id, runtime->config.executor, std::move(inputs),
      [this, node_id](const std::string& port, const Packet& packet) {
        return Emit(node_id, port, packet);
      },
      &side_packets_, &services_, resources_.get(), &metrics_, &cancelled_);
  Status status;
  if (runtime->node->concurrency() == ConcurrencyMode::kReentrant)
    status = runtime->node->Process(context);
  else {
    std::lock_guard<std::mutex> lock(runtime->process_mutex);
    status = runtime->node->Process(context);
  }
  const std::int64_t duration = NowMicros() - start;
  metrics_.Increment("node." + node_id + ".processed");
  metrics_.SetGauge("node." + node_id + ".last_latency_us", static_cast<double>(duration));
  trace_.Add({"Process", "node", node_id, runtime->config.executor, start, duration, ThreadId()});
  if (!status.ok()) SetError(Status(status.code(), "node " + node_id + ": " + status.message()));
}

Scheduler* GraphRunner::ExecutorFor(const std::string& node_id) {
  const auto node = nodes_.find(node_id);
  if (node == nodes_.end()) return nullptr;
  const auto executor = executors_.find(node->second->config.executor);
  return executor == executors_.end() ? nullptr : executor->second.get();
}

void GraphRunner::SetError(const Status& status) {
  std::shared_ptr<ErrorCallbackRuntime> callback_runtime;
  {
    std::lock_guard<std::mutex> lock(error_mutex_);
    if (!last_error_.ok()) return;
    last_error_ = status;
    callback_runtime = error_callback_runtime_;
  }
  metrics_.Increment("graph.errors");
  RKAVP_LOG(Error) << status.message();
  cancelled_ = true;
  CloseQueues();
  state_condition_.notify_all();
  if (callback_runtime) {
    std::lock_guard<std::mutex> lock(callback_runtime->mutex);
    if (!callback_runtime->stopped) {
      callback_runtime->queue.push_back(status);
      callback_runtime->condition.notify_one();
    }
  }
}

void GraphRunner::CloseQueues() {
  for (auto& input : graph_inputs_) input.second->queue->Close();
  for (auto& edge : edges_) edge->queue->Close();
}

bool GraphRunner::BeginOperation() const {
  std::lock_guard<std::mutex> lock(lifecycle_mutex_);
  if (stopping_ || !running_) return false;
  ++active_operations_;
  return true;
}

void GraphRunner::EndOperation() const {
  std::lock_guard<std::mutex> lock(lifecycle_mutex_);
  if (active_operations_ != 0) --active_operations_;
  lifecycle_condition_.notify_all();
}

Status GraphRunner::AddSource(const std::string& slot, const std::string& source_id,
                              ConfigValue options) {
  return SubmitControl([this, slot, source_id, options = std::move(options)]() mutable {
    return AddSourceImpl(slot, source_id, std::move(options));
  });
}

Status GraphRunner::SubmitControl(std::function<Status()> task) {
  if (!running_ || !control_executor_)
    return Status::FailedPrecondition("graph control executor is not running");
  auto result = std::make_shared<std::promise<Status>>();
  auto future = result->get_future();
  Status status = control_executor_->Post([task = std::move(task), result]() mutable {
    try {
      result->set_value(task());
    } catch (const std::exception& error) {
      result->set_value(Status::Internal(error.what()));
    } catch (...) {
      result->set_value(Status::Internal("control operation failed"));
    }
  });
  if (!status.ok()) return status;
  return future.get();
}

Status GraphRunner::AddSourceImpl(const std::string& slot, const std::string& source_id,
                                  ConfigValue options) {
  if (source_id.empty()) return Status::Invalid("source id is required");
  const auto slot_it = source_slots_.find(slot);
  if (slot_it == source_slots_.end()) return Status::NotFound("source slot not found: " + slot);
  if (graph_inputs_.find(slot_it->second.input_stream) == graph_inputs_.end()) {
    return Status::NotFound("source slot input stream not found: " + slot_it->second.input_stream);
  }
  SourceRuntime source;
  source.slot = slot;
  source.input_stream = slot_it->second.input_stream;
  source.source_id = source_id;
  source.options = std::move(options);
  source.health.state = StreamHealthState::kConnecting;
  if (!slot_it->second.subgraph_template.empty()) {
    GraphConfig child_config;
    Status status = YamlGraphLoader::LoadFile(slot_it->second.subgraph_template, &child_config);
    if (!status.ok()) return Status::Invalid("source template " + slot + ": " + status.message());
    if (source.options.Is<std::shared_ptr<ConfigValue::Object>>()) {
      const auto& overrides = source.options.AsObject();
      for (auto& node : child_config.nodes) {
        for (const auto& override_value : overrides) {
          const std::string prefix = node.id + ".";
          if (override_value.first.rfind(prefix, 0) == 0) {
            node.options[override_value.first.substr(prefix.size())] = override_value.second;
          } else if (child_config.nodes.size() == 1 &&
                     override_value.first.find('.') == std::string::npos) {
            node.options[override_value.first] = override_value.second;
          }
        }
      }
    }
    Graph child_graph(std::move(child_config), registry_);
    status = child_graph.Validate();
    if (!status.ok()) return Status::Invalid("source template " + slot + ": " + status.message());
    source.subgraph = std::make_unique<GraphRunner>(std::move(child_graph), registry_);
    ObserverHandle observer;
    status = source.subgraph->ObserveOutput(
        slot_it->second.output_stream,
        [this, source_id](const Packet& input) {
          Packet packet = input;
          packet.mutable_metadata().Set("source_id", source_id);
          Status emit_status = AddSourcePacket(source_id, std::move(packet));
          if (!emit_status.ok() && emit_status.code() != StatusCode::kCancelled)
            SetError(emit_status);
        },
        {}, &observer);
    if (!status.ok()) return status;
  }
  GraphRunner* child = source.subgraph.get();
  {
    std::lock_guard<std::mutex> lock(source_mutex_);
    if (sources_.count(source_id) != 0)
      return Status::AlreadyExists("source already exists: " + source_id);
    sources_.emplace(source_id, std::move(source));
    ++active_sources_;
  }
  if (child != nullptr) {
    Status status = child->Start();
    if (!status.ok()) {
      std::lock_guard<std::mutex> lock(source_mutex_);
      sources_.erase(source_id);
      --active_sources_;
      state_condition_.notify_all();
      return Status::Internal("source subgraph start failed: " + status.message());
    }
  }
  metrics_.Increment("sources.added");
  return Status::Ok();
}

Status GraphRunner::AddSourcePacket(const std::string& source_id, Packet packet) {
  std::string input_stream;
  std::shared_ptr<SourceRuntime::DrainState> drain;
  {
    std::lock_guard<std::mutex> lock(source_mutex_);
    const auto it = sources_.find(source_id);
    if (it == sources_.end()) return Status::NotFound("source not found: " + source_id);
    input_stream = it->second.input_stream;
    drain = it->second.drain;
    if (packet.event() == ControlEvent::kEndOfStream) {
      if (it->second.active) {
        it->second.active = false;
        --active_sources_;
      }
      it->second.health.state = StreamHealthState::kEos;
      state_condition_.notify_all();
      return Status::Ok();
    }
    if (packet.event() == ControlEvent::kTimestampBound) return Status::Ok();
    it->second.health.state = StreamHealthState::kStreaming;
    it->second.health.last_packet_time_us = clock_->NowMicros();
  }
  {
    std::lock_guard<std::mutex> lock(drain->mutex);
    if (!drain->accepting) {
      return Status::Cancelled("source is draining: " + source_id);
    }
    ++drain->in_flight;
  }
  packet.AddLifetimeToken(std::shared_ptr<void>(reinterpret_cast<void*>(1), [drain](void*) {
    std::lock_guard<std::mutex> lock(drain->mutex);
    if (drain->in_flight != 0) --drain->in_flight;
    drain->condition.notify_all();
  }));
  packet.mutable_metadata().Set("source_id", source_id);
  return AddPacket(input_stream, std::move(packet));
}

Status GraphRunner::RemoveSource(const std::string& source_id, std::int64_t drain_timeout_ms) {
  return SubmitControl([this, source_id, drain_timeout_ms] {
    return RemoveSourceImpl(source_id, drain_timeout_ms);
  });
}

Status GraphRunner::RemoveSourceImpl(const std::string& source_id, std::int64_t drain_timeout_ms) {
  GraphRunner* child = nullptr;
  std::shared_ptr<SourceRuntime::DrainState> drain;
  {
    std::lock_guard<std::mutex> lock(source_mutex_);
    const auto it = sources_.find(source_id);
    if (it == sources_.end()) return Status::NotFound("source not found: " + source_id);
    it->second.health.state = StreamHealthState::kEos;
    child = it->second.subgraph.get();
    drain = it->second.drain;
  }
  {
    std::lock_guard<std::mutex> lock(drain->mutex);
    drain->accepting = false;
  }
  if (child != nullptr) child->Stop();
  std::unique_lock<std::mutex> drain_lock(drain->mutex);
  const auto drained = [drain] { return drain->in_flight == 0; };
  bool completed = drained();
  if (!completed && drain_timeout_ms < 0) {
    drain->condition.wait(drain_lock, drained);
    completed = true;
  } else if (!completed) {
    completed =
        drain->condition.wait_for(drain_lock, std::chrono::milliseconds(drain_timeout_ms), drained);
  }
  drain_lock.unlock();
  if (!completed) {
    std::size_t in_flight = 0;
    {
      std::lock_guard<std::mutex> lock(drain->mutex);
      in_flight = drain->in_flight;
    }
    std::lock_guard<std::mutex> lock(source_mutex_);
    const auto it = sources_.find(source_id);
    if (it != sources_.end()) {
      it->second.health.state = StreamHealthState::kStalled;
      it->second.health.message =
          "source drain timed out with " + std::to_string(in_flight) + " packet(s) in flight";
    }
    return Status::Unavailable("source drain timed out: " + source_id);
  }
  std::lock_guard<std::mutex> lock(source_mutex_);
  const auto source = sources_.find(source_id);
  if (source != sources_.end() && source->second.active) --active_sources_;
  sources_.erase(source_id);
  state_condition_.notify_all();
  metrics_.Increment("sources.removed");
  return Status::Ok();
}

Status GraphRunner::RestartSource(const std::string& source_id) {
  return SubmitControl([this, source_id] { return RestartSourceImpl(source_id); });
}

Status GraphRunner::RestartSourceImpl(const std::string& source_id) {
  GraphRunner* child = nullptr;
  std::shared_ptr<SourceRuntime::DrainState> drain;
  {
    std::lock_guard<std::mutex> lock(source_mutex_);
    const auto it = sources_.find(source_id);
    if (it == sources_.end()) return Status::NotFound("source not found: " + source_id);
    it->second.health.state = StreamHealthState::kReconnecting;
    ++it->second.health.reconnect_count;
    child = it->second.subgraph.get();
    drain = it->second.drain;
  }
  Status status = Status::Ok();
  if (child != nullptr) {
    child->Stop();
    status = child->Start();
  }
  {
    std::lock_guard<std::mutex> lock(source_mutex_);
    const auto it = sources_.find(source_id);
    if (it == sources_.end()) return Status::Cancelled("source was removed during restart");
    if (status.ok()) {
      it->second.health.state =
          child == nullptr ? StreamHealthState::kReconnecting : StreamHealthState::kConnecting;
      if (!it->second.active) {
        it->second.active = true;
        ++active_sources_;
      }
      std::lock_guard<std::mutex> drain_lock(drain->mutex);
      drain->accepting = true;
    } else {
      it->second.health.state = StreamHealthState::kFailed;
      it->second.health.message = status.message();
    }
  }
  return status;
}

std::vector<SourceInfo> GraphRunner::ListSources() const {
  std::lock_guard<std::mutex> lock(source_mutex_);
  std::vector<SourceInfo> result;
  result.reserve(sources_.size());
  for (const auto& item : sources_)
    result.push_back({item.second.slot, item.first, item.second.health});
  return result;
}

StreamHealth GraphRunner::GetSourceHealth(const std::string& source_id) const {
  std::lock_guard<std::mutex> lock(source_mutex_);
  const auto it = sources_.find(source_id);
  if (it == sources_.end()) return {StreamHealthState::kFailed, 0, 0, 0, "source not found"};
  return it->second.health;
}

GraphRuntimeInfo GraphRunner::GetRuntimeInfo() const {
  GraphRuntimeInfo info;
  info.running = running_;
  info.cancelled = cancelled_;
  if (!BeginOperation()) return info;
  ScopeExit operation([this] { EndOperation(); });
  for (const auto& item : nodes_) {
    NodeRuntimeInfo node;
    node.id = item.first;
    node.executor = item.second->config.executor;
    node.state = std::to_string(static_cast<int>(item.second->node->state()));
    {
      std::lock_guard<std::mutex> lock(item.second->input_mutex);
      for (const auto& pending : item.second->pending)
        node.pending_packets += pending.second.size();
    }
    info.nodes.push_back(std::move(node));
  }
  for (const auto& item : executors_) {
    info.executors.push_back({item.first, item.second->queued(), item.second->active(),
                              item.second->starvation_events(), item.second->max_wait_us()});
  }
  if (control_executor_) {
    info.executors.push_back({"control", control_executor_->queued(), control_executor_->active(),
                              control_executor_->starvation_events(),
                              control_executor_->max_wait_us()});
  }
  for (const auto& item : graph_inputs_) {
    std::lock_guard<std::mutex> lock(item.second->state_mutex);
    info.streams.push_back({item.first, item.second->open, item.second->last_timestamp,
                            item.second->bound, item.second->queue->size(),
                            item.second->dropped.load()});
  }
  for (const auto& edge : edges_) {
    std::lock_guard<std::mutex> lock(edge->state_mutex);
    info.streams.push_back({edge->config.from_node + "." + edge->config.from_port + "->" +
                                edge->config.to_node + "." + edge->config.to_port,
                            !edge->closed, edge->last_timestamp, edge->bound, edge->queue->size(),
                            edge->dropped.load()});
  }
  return info;
}

}  // namespace rkavp
