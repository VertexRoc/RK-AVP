#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "rkavp/bounded_queue.hpp"
#include "rkavp/clock.hpp"
#include "rkavp/graph.hpp"
#include "rkavp/hardware_context.hpp"
#include "rkavp/metrics.hpp"
#include "rkavp/node_registry.hpp"
#include "rkavp/runtime_info.hpp"
#include "rkavp/scheduler.hpp"
#include "rkavp/services.hpp"
#include "rkavp/trace.hpp"

namespace rkavp {

struct ObserverHandle {
  std::uint64_t id = 0;
  explicit operator bool() const { return id != 0; }
};

struct ObserverOptions {
  std::size_t queue_capacity = 64;
  QueuePolicy queue_policy = QueuePolicy::kDropOldest;
  bool observe_timestamp_bounds = false;
};

class GraphRunner {
 public:
  using OutputCallback = std::function<void(const Packet&)>;
  using ErrorCallback = std::function<void(const Status&)>;

  GraphRunner(Graph graph, const NodeRegistry* registry);
  ~GraphRunner();

  Status SetSidePacket(std::string name, Packet packet);
  GraphServiceRegistry& services() { return services_; }
  void SetResourceManager(std::shared_ptr<ResourceManager> resources);
  Status ObserveOutput(const std::string& stream, OutputCallback callback);
  Status ObserveOutput(const std::string& stream, OutputCallback callback, ObserverOptions options,
                       ObserverHandle* handle);
  Status CancelObserver(ObserverHandle handle);
  Status WaitForObservedOutput(ObserverHandle handle, std::int64_t timeout_ms = -1);
  Status SetErrorCallback(ErrorCallback callback);
  void SetClock(std::shared_ptr<IClock> clock);

  Status Start();
  Status AddPacket(const std::string& stream, Packet packet);
  Status SetInputTimestampBound(const std::string& stream, Timestamp bound);
  Status CloseInputStream(const std::string& stream);
  Status AddSource(const std::string& slot, const std::string& source_id, ConfigValue options = {});
  Status AddSourcePacket(const std::string& source_id, Packet packet);
  Status RemoveSource(const std::string& source_id, std::int64_t drain_timeout_ms = 1000);
  Status RestartSource(const std::string& source_id);
  std::vector<SourceInfo> ListSources() const;
  StreamHealth GetSourceHealth(const std::string& source_id) const;
  Status WaitUntilIdle(std::int64_t timeout_ms = -1);
  Status WaitUntilDone(std::int64_t timeout_ms = -1);
  void Cancel();
  void Stop();

  bool running() const { return running_; }
  bool cancelled() const { return cancelled_; }
  Status last_error() const;
  MetricsRegistry& metrics() { return metrics_; }
  const MetricsRegistry& metrics() const { return metrics_; }
  TraceBuffer& trace() { return trace_; }
  const TraceBuffer& trace() const { return trace_; }
  GraphRuntimeInfo GetRuntimeInfo() const;

 private:
  struct RuntimeNode {
    NodeConfig config;
    NodeContract contract;
    std::unique_ptr<Node> node;
    std::unique_ptr<NodeContext> start_context;
    std::mutex input_mutex;
    std::mutex process_mutex;
    std::unordered_map<std::string, Packet> latest;
    std::unordered_map<std::string, std::map<std::int64_t, Packet>> pending;
    std::unordered_set<std::string> closed_ports;
    std::unordered_map<std::string, Timestamp> bounds;
    bool eos_dispatched = false;
  };

  struct EdgeRuntime {
    EdgeConfig config;
    std::unique_ptr<BoundedQueue<Packet>> queue;
    std::atomic<bool> scheduled{false};
    Timestamp last_timestamp = Timestamp::Unset();
    Timestamp bound = Timestamp::Unset();
    bool closed = false;
    std::atomic<std::uint64_t> dropped{0};
    mutable std::mutex state_mutex;
  };

  struct InputRuntime {
    GraphPortConfig config;
    std::unique_ptr<BoundedQueue<Packet>> queue;
    std::atomic<bool> scheduled{false};
    Timestamp last_timestamp = Timestamp::Unset();
    Timestamp bound = Timestamp::Unset();
    std::atomic<std::uint64_t> dropped{0};
    bool open = true;
    mutable std::mutex state_mutex;
  };

  struct ObserverRuntime {
    ObserverHandle handle;
    std::string stream;
    OutputCallback callback;
    ObserverOptions options;
    std::mutex mutex;
    std::condition_variable condition;
    std::deque<Packet> queue;
    std::thread worker;
    bool stopped = false;
    bool active = false;
    std::uint64_t delivered = 0;
    std::uint64_t waited = 0;
    std::uint64_t dropped = 0;
  };

  struct SourceRuntime {
    std::string slot;
    std::string input_stream;
    std::string source_id;
    ConfigValue options;
    StreamHealth health;
    std::unique_ptr<GraphRunner> subgraph;
    struct DrainState {
      std::mutex mutex;
      std::condition_variable condition;
      std::size_t in_flight = 0;
      bool accepting = true;
    };
    std::shared_ptr<DrainState> drain = std::make_shared<DrainState>();
    bool active = true;
  };

  struct ErrorCallbackRuntime {
    ErrorCallback callback;
    std::mutex mutex;
    std::condition_variable condition;
    std::deque<Status> queue;
    std::thread worker;
    bool stopped = false;
  };

  Status Emit(const std::string& node_id, const std::string& output_port, const Packet& packet);
  Status EnqueueEdge(std::size_t edge_index, Packet packet);
  void ScheduleEdge(std::size_t edge_index);
  void DrainEdge(std::size_t edge_index);
  void ScheduleInput(const std::string& stream);
  void DrainInput(const std::string& stream);
  Status ValidateStreamPacket(Timestamp timestamp, Timestamp* last_timestamp, Timestamp bound,
                              bool closed, const std::string& stream) const;
  void EnqueueObserver(const std::shared_ptr<ObserverRuntime>& observer, const Packet& packet);
  void RunObserver(const std::shared_ptr<ObserverRuntime>& observer);
  void ShutdownObservers();
  void ShutdownErrorCallback();
  Status SubmitControl(std::function<Status()> task);
  Status AddSourceImpl(const std::string& slot, const std::string& source_id, ConfigValue options);
  Status RemoveSourceImpl(const std::string& source_id, std::int64_t drain_timeout_ms);
  Status RestartSourceImpl(const std::string& source_id);
  Status Deliver(const std::string& node_id, const std::string& port, Packet packet);
  bool PrepareInvocation(RuntimeNode* runtime, const std::string& port, Packet packet,
                         PacketSet* inputs);
  void PruneImpossibleSyncPacketsLocked(RuntimeNode* runtime);
  void Invoke(const std::string& node_id, PacketSet inputs);
  Scheduler* ExecutorFor(const std::string& node_id);
  void SetError(const Status& status);
  void CloseQueues();
  bool BeginOperation() const;
  void EndOperation() const;

  Graph graph_;
  const NodeRegistry* registry_;
  std::atomic<bool> running_{false};
  std::atomic<bool> cancelled_{false};
  std::unordered_map<std::string, std::unique_ptr<Scheduler>> executors_;
  std::unordered_map<std::string, std::unique_ptr<RuntimeNode>> nodes_;
  std::vector<std::string> lifecycle_order_;
  std::vector<std::unique_ptr<EdgeRuntime>> edges_;
  std::unordered_map<std::string, std::vector<std::size_t>> routes_;
  std::unordered_map<std::string, std::unique_ptr<InputRuntime>> graph_inputs_;
  std::unordered_map<std::string, std::vector<std::string>> graph_outputs_;
  std::unordered_map<std::string, std::vector<std::shared_ptr<ObserverRuntime>>> observers_;
  std::unordered_map<std::uint64_t, std::shared_ptr<ObserverRuntime>> observer_handles_;
  std::vector<std::shared_ptr<ObserverRuntime>> retired_observers_;
  std::atomic<std::uint64_t> next_observer_id_{1};
  std::unordered_set<std::string> open_inputs_;
  std::unordered_set<std::string> open_source_nodes_;
  SidePacketSet side_packets_;
  GraphServiceRegistry services_;
  std::shared_ptr<ResourceManager> resources_ = std::make_shared<FileResourceManager>();
  MetricsRegistry metrics_;
  TraceBuffer trace_;
  mutable std::mutex state_mutex_;
  std::condition_variable state_condition_;
  mutable std::mutex error_mutex_;
  Status last_error_;
  std::shared_ptr<ErrorCallbackRuntime> error_callback_runtime_;
  std::shared_ptr<IClock> clock_ = std::make_shared<SteadyClock>();
  std::shared_ptr<HardwareContextService> hardware_context_ =
      std::make_shared<HardwareContextService>();
  std::unordered_map<std::string, SourceRuntime> sources_;
  std::unordered_map<std::string, SourceSlotConfig> source_slots_;
  std::unique_ptr<Scheduler> control_executor_;
  mutable std::mutex source_mutex_;
  mutable std::mutex lifecycle_mutex_;
  mutable std::recursive_mutex transition_mutex_;
  mutable std::condition_variable lifecycle_condition_;
  mutable std::size_t active_operations_ = 0;
  std::atomic<std::size_t> active_sources_{0};
  bool stopping_ = false;
};

}  // namespace rkavp
