#pragma once

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "rkavp/node.hpp"

namespace rkavp::testing {

class NodeTestRunner {
 public:
  explicit NodeTestRunner(std::unique_ptr<Node> node, std::string node_id = "node_under_test",
                          std::string executor = "test");
  ~NodeTestRunner();

  NodeTestRunner(const NodeTestRunner&) = delete;
  NodeTestRunner& operator=(const NodeTestRunner&) = delete;

  Status Configure(ConfigValue options = {});
  Status SetInput(std::string_view port, Packet packet);
  Status SetSidePacket(std::string_view name, Packet packet);

  template <typename T>
  Status SetService(std::string_view name, std::shared_ptr<T> service) {
    return services_.Set<T>(std::string(name), std::move(service));
  }

  void SetResourceManager(std::shared_ptr<ResourceManager> resources);

  Status Start();
  Status Process();
  Status Stop();
  Status Close();
  Status RunOnce();

  std::vector<Packet> Outputs(std::string_view port) const;
  std::vector<Packet> TakeOutputs(std::string_view port);
  Status WaitForOutput(std::string_view port, std::size_t count, std::int64_t timeout_ms);

  void Cancel();
  NodeState state() const;
  MetricsRegistry& metrics() { return metrics_; }
  const MetricsRegistry& metrics() const { return metrics_; }

 private:
  Status CaptureOutput(const std::string& port, Packet packet);
  std::unique_ptr<NodeContext> MakeContext(PacketSet inputs);
  Status CleanupAfterFailure(Status status);

  std::unique_ptr<Node> node_;
  std::string node_id_;
  std::string executor_;
  NodeContract contract_;
  PacketSet inputs_;
  SidePacketSet side_packets_;
  GraphServiceRegistry services_;
  std::shared_ptr<ResourceManager> resources_;
  MetricsRegistry metrics_;
  std::atomic<bool> cancelled_{false};
  std::unique_ptr<NodeContext> start_context_;

  mutable std::mutex output_mutex_;
  std::condition_variable output_condition_;
  std::unordered_map<std::string, std::vector<Packet>> outputs_;
};

}  // namespace rkavp::testing
