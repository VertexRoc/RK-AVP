#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include <pybind11/pybind11.h>
#include <pybind11/stl.h>

#include "rkavp/core.hpp"

namespace py = pybind11;

namespace {

void ThrowOnError(const rkavp::Status& status) {
  if (!status.ok()) throw std::runtime_error(status.message());
}

rkavp::ConfigValue ConfigFromPython(const py::handle& value) {
  if (value.is_none()) return {};
  if (py::isinstance<py::bool_>(value)) return value.cast<bool>();
  if (py::isinstance<py::int_>(value)) return value.cast<std::int64_t>();
  if (py::isinstance<py::float_>(value)) return value.cast<double>();
  if (py::isinstance<py::str>(value)) return value.cast<std::string>();
  if (py::isinstance<py::dict>(value)) {
    rkavp::ConfigValue::Object object;
    for (const auto& item : value.cast<py::dict>()) {
      object.emplace(py::cast<std::string>(item.first), ConfigFromPython(item.second));
    }
    return object;
  }
  if (py::isinstance<py::list>(value) || py::isinstance<py::tuple>(value)) {
    rkavp::ConfigValue::Array array;
    for (const auto& item : value) array.push_back(ConfigFromPython(item));
    return array;
  }
  throw py::type_error("packet value must be bool, int, float, str, list, dict, or None");
}

py::object ConfigToPython(const rkavp::ConfigValue& value) {
  if (value.is_null()) return py::none();
  if (value.Is<bool>()) return py::bool_(value.As<bool>());
  if (value.Is<std::int64_t>()) return py::int_(value.As<std::int64_t>());
  if (value.Is<double>()) return py::float_(value.As<double>());
  if (value.Is<std::string>()) return py::str(value.As<std::string>());
  if (value.Is<std::shared_ptr<rkavp::ConfigValue::Array>>()) {
    py::list result;
    for (const auto& item : value.AsArray()) result.append(ConfigToPython(item));
    return std::move(result);
  }
  py::dict result;
  for (const auto& item : value.AsObject()) result[py::str(item.first)] = ConfigToPython(item.second);
  return std::move(result);
}

py::dict PacketToPython(const rkavp::Packet& packet) {
  py::dict result;
  result["timestamp_us"] = packet.timestamp().is_range_value()
                               ? py::cast(packet.timestamp().microseconds())
                               : py::none();
  result["event"] = static_cast<int>(packet.event());
  result["value"] = packet.Is<rkavp::ConfigValue>()
                        ? ConfigToPython(packet.Get<rkavp::ConfigValue>())
                        : py::none();
  return result;
}

const char* HealthStateName(rkavp::StreamHealthState state) {
  switch (state) {
    case rkavp::StreamHealthState::kConnecting: return "connecting";
    case rkavp::StreamHealthState::kStreaming: return "streaming";
    case rkavp::StreamHealthState::kStalled: return "stalled";
    case rkavp::StreamHealthState::kReconnecting: return "reconnecting";
    case rkavp::StreamHealthState::kEos: return "eos";
    case rkavp::StreamHealthState::kFailed: return "failed";
    case rkavp::StreamHealthState::kStopped: return "stopped";
  }
  return "unknown";
}

py::dict HealthToPython(const rkavp::StreamHealth& health) {
  py::dict result;
  result["state"] = HealthStateName(health.state);
  result["last_packet_time_us"] = health.last_packet_time_us;
  result["reconnect_count"] = health.reconnect_count;
  result["dropped_packets"] = health.dropped_packets;
  result["message"] = health.message;
  return result;
}

class PythonObserver {
 public:
  PythonObserver(py::function callback, std::size_t capacity, std::atomic<std::uint64_t>* errors,
                 std::atomic<std::uint64_t>* dropped)
      : callback_(std::move(callback)), capacity_(capacity), errors_(errors), dropped_(dropped),
        thread_([this] { Run(); }) {}

  ~PythonObserver() { Shutdown(); }

  void Enqueue(const rkavp::Packet& packet) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (stopped_) return;
    if (queue_.size() == capacity_) {
      queue_.pop_front();
      ++(*dropped_);
    }
    queue_.push_back(packet);
    condition_.notify_one();
  }

  void Shutdown() {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      stopped_ = true;
      condition_.notify_all();
    }
    if (thread_.joinable()) thread_.join();
  }

 private:
  void Run() {
    while (true) {
      rkavp::Packet packet;
      {
        std::unique_lock<std::mutex> lock(mutex_);
        condition_.wait(lock, [this] { return stopped_ || !queue_.empty(); });
        if (stopped_ && queue_.empty()) return;
        packet = std::move(queue_.front());
        queue_.pop_front();
      }
      py::gil_scoped_acquire acquire;
      try {
        callback_(PacketToPython(packet));
      } catch (py::error_already_set& error) {
        ++(*errors_);
        error.restore();
        PyErr_Clear();
      }
    }
  }

  py::function callback_;
  std::size_t capacity_;
  std::atomic<std::uint64_t>* errors_;
  std::atomic<std::uint64_t>* dropped_;
  std::mutex mutex_;
  std::condition_variable condition_;
  std::deque<rkavp::Packet> queue_;
  bool stopped_ = false;
  std::thread thread_;
};

class GraphSession {
 public:
  explicit GraphSession(const std::string& path) {
    ThrowOnError(rkavp::YamlGraphLoader::LoadFile(path, &config_));
    rkavp::RegisterBuiltinNodes(&registry_);
    rkavp::Graph graph(config_, &registry_);
    ThrowOnError(graph.Validate());
    inspection_ = graph.Inspect();
    runner_ = std::make_unique<rkavp::GraphRunner>(std::move(graph), &registry_);
  }

  ~GraphSession() {
    Stop();
    py::gil_scoped_release release;
    for (const auto& observer : observers_) observer->Shutdown();
  }

  void Start() { ThrowOnError(runner_->Start()); }
  void Stop() {
    if (!runner_) return;
    py::gil_scoped_release release;
    runner_->Stop();
  }
  void Cancel() {
    py::gil_scoped_release release;
    runner_->Cancel();
  }
  void SetSidePacket(const std::string& name, const py::object& value) {
    ThrowOnError(runner_->SetSidePacket(
        name, rkavp::Packet::Make(ConfigFromPython(value), rkavp::Timestamp::FromMicroseconds(0))));
  }
  void AddPacket(const std::string& stream, const py::object& value, std::int64_t timestamp_us) {
    ThrowOnError(runner_->AddPacket(
        stream, rkavp::Packet::Make(ConfigFromPython(value), rkavp::Timestamp::FromMicroseconds(timestamp_us))));
  }
  void SetInputTimestampBound(const std::string& stream, std::int64_t timestamp_us) {
    ThrowOnError(runner_->SetInputTimestampBound(
        stream, rkavp::Timestamp::FromMicroseconds(timestamp_us)));
  }
  void CloseInput(const std::string& stream) { ThrowOnError(runner_->CloseInputStream(stream)); }
  void AddSource(const std::string& slot, const std::string& source_id, const py::object& options) {
    ThrowOnError(runner_->AddSource(slot, source_id, ConfigFromPython(options)));
  }
  void AddSourcePacket(const std::string& source_id, const py::object& value,
                       std::int64_t timestamp_us) {
    ThrowOnError(runner_->AddSourcePacket(
        source_id, rkavp::Packet::Make(ConfigFromPython(value),
                                      rkavp::Timestamp::FromMicroseconds(timestamp_us))));
  }
  void RemoveSource(const std::string& source_id, std::int64_t drain_timeout_ms) {
    py::gil_scoped_release release;
    ThrowOnError(runner_->RemoveSource(source_id, drain_timeout_ms));
  }
  void RestartSource(const std::string& source_id) {
    ThrowOnError(runner_->RestartSource(source_id));
  }
  py::list ListSources() const {
    py::list result;
    for (const auto& source : runner_->ListSources()) {
      py::dict item;
      item["slot"] = source.slot;
      item["source_id"] = source.source_id;
      item["health"] = HealthToPython(source.health);
      result.append(std::move(item));
    }
    return result;
  }
  py::dict SourceHealth(const std::string& source_id) const {
    return HealthToPython(runner_->GetSourceHealth(source_id));
  }
  void WaitUntilIdle(std::int64_t timeout_ms) {
    py::gil_scoped_release release;
    ThrowOnError(runner_->WaitUntilIdle(timeout_ms));
  }
  void WaitUntilDone(std::int64_t timeout_ms) {
    py::gil_scoped_release release;
    ThrowOnError(runner_->WaitUntilDone(timeout_ms));
  }
  void ObserveOutput(const std::string& stream, py::function callback, std::size_t queue_capacity) {
    if (queue_capacity == 0) throw py::value_error("queue_capacity must be positive");
    auto observer = std::make_shared<PythonObserver>(std::move(callback), queue_capacity,
                                                     &callback_errors_, &observer_dropped_);
    ThrowOnError(runner_->ObserveOutput(stream, [observer](const rkavp::Packet& packet) {
      observer->Enqueue(packet);
    }));
    observers_.push_back(std::move(observer));
  }
  void SetErrorCallback(py::function callback) {
    error_callback_ = std::move(callback);
    ThrowOnError(runner_->SetErrorCallback([this](const rkavp::Status& status) {
      py::gil_scoped_acquire acquire;
      try {
        error_callback_(static_cast<int>(status.code()), status.message());
      } catch (py::error_already_set& error) {
        ++callback_errors_;
        error.restore();
        PyErr_Clear();
      }
    }));
  }
  bool running() const { return runner_ && runner_->running(); }
  bool cancelled() const { return runner_ && runner_->cancelled(); }
  const std::string& inspect() const { return inspection_; }
  std::string metrics() const { return runner_->metrics().ExportText(); }
  std::string trace_json() const { return runner_->trace().ToChromeTraceJson(); }
  py::dict runtime_info() const {
    const auto info = runner_->GetRuntimeInfo();
    py::dict result;
    result["running"] = info.running;
    result["cancelled"] = info.cancelled;
    py::list nodes;
    for (const auto& node : info.nodes) {
      py::dict item;
      item["id"] = node.id;
      item["executor"] = node.executor;
      item["state"] = node.state;
      item["pending_packets"] = node.pending_packets;
      nodes.append(std::move(item));
    }
    py::list executors;
    for (const auto& executor : info.executors) {
      py::dict item;
      item["name"] = executor.name;
      item["queued"] = executor.queued;
      item["active"] = executor.active;
      item["starvation_events"] = executor.starvation_events;
      item["max_wait_us"] = executor.max_wait_us;
      executors.append(std::move(item));
    }
    py::list streams;
    for (const auto& stream : info.streams) {
      py::dict item;
      item["name"] = stream.name;
      item["open"] = stream.open;
      item["last_timestamp_us"] = stream.last_timestamp.is_range_value()
                                         ? py::cast(stream.last_timestamp.microseconds()) : py::none();
      item["bound_us"] = stream.bound.is_range_value()
                               ? py::cast(stream.bound.microseconds()) : py::none();
      item["queue_depth"] = stream.queue_depth;
      item["dropped"] = stream.dropped;
      streams.append(std::move(item));
    }
    result["nodes"] = std::move(nodes);
    result["executors"] = std::move(executors);
    result["streams"] = std::move(streams);
    return result;
  }
  std::uint64_t callback_errors() const { return callback_errors_; }
  std::uint64_t observer_dropped() const { return observer_dropped_; }

 private:
  rkavp::NodeRegistry registry_;
  rkavp::GraphConfig config_;
  std::string inspection_;
  std::unique_ptr<rkavp::GraphRunner> runner_;
  py::function error_callback_;
  std::atomic<std::uint64_t> callback_errors_{0};
  std::atomic<std::uint64_t> observer_dropped_{0};
  std::vector<std::shared_ptr<PythonObserver>> observers_;
};

}  // namespace

PYBIND11_MODULE(_rkavp, module) {
  module.doc() = "RK-AVP graph control bindings; media payloads remain in C++";
  module.def("validate", [](const std::string& path) {
    rkavp::GraphConfig config;
    ThrowOnError(rkavp::YamlGraphLoader::LoadFile(path, &config));
    rkavp::NodeRegistry registry;
    rkavp::RegisterBuiltinNodes(&registry);
    ThrowOnError(rkavp::Graph(std::move(config), &registry).Validate());
    return true;
  });
  py::class_<GraphSession>(module, "Graph")
      .def(py::init<const std::string&>())
      .def("start", &GraphSession::Start)
      .def("stop", &GraphSession::Stop)
      .def("cancel", &GraphSession::Cancel)
      .def("set_side_packet", &GraphSession::SetSidePacket)
      .def("add_packet", &GraphSession::AddPacket, py::arg("stream"), py::arg("value"),
           py::arg("timestamp_us"))
      .def("set_input_timestamp_bound", &GraphSession::SetInputTimestampBound,
           py::arg("stream"), py::arg("timestamp_us"))
      .def("close_input", &GraphSession::CloseInput)
      .def("add_source", &GraphSession::AddSource, py::arg("slot"), py::arg("source_id"),
           py::arg("options") = py::dict())
      .def("add_source_packet", &GraphSession::AddSourcePacket, py::arg("source_id"),
           py::arg("value"), py::arg("timestamp_us"))
      .def("remove_source", &GraphSession::RemoveSource, py::arg("source_id"),
           py::arg("drain_timeout_ms") = 1000)
      .def("restart_source", &GraphSession::RestartSource)
      .def("list_sources", &GraphSession::ListSources)
      .def("source_health", &GraphSession::SourceHealth)
      .def("wait_until_idle", &GraphSession::WaitUntilIdle, py::arg("timeout_ms") = -1)
      .def("wait_until_done", &GraphSession::WaitUntilDone, py::arg("timeout_ms") = -1)
      .def("observe_output", &GraphSession::ObserveOutput, py::arg("stream"), py::arg("callback"),
           py::arg("queue_capacity") = 64)
      .def("set_error_callback", &GraphSession::SetErrorCallback)
      .def_property_readonly("running", &GraphSession::running)
      .def_property_readonly("cancelled", &GraphSession::cancelled)
      .def_property_readonly("inspection", &GraphSession::inspect)
      .def_property_readonly("metrics", &GraphSession::metrics)
      .def_property_readonly("trace_json", &GraphSession::trace_json)
      .def_property_readonly("runtime_info", &GraphSession::runtime_info)
      .def_property_readonly("callback_errors", &GraphSession::callback_errors)
      .def_property_readonly("observer_dropped", &GraphSession::observer_dropped);
}
