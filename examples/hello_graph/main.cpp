#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <iostream>
#include <mutex>
#include <string>
#include <utility>

#include "rkavp/core.hpp"

namespace {

int Fail(const rkavp::Status& status) {
  std::cerr << "error: " << status.message() << '\n';
  return 1;
}

}  // namespace

int main(int argc, char** argv) {
  const std::string graph_path = argc > 1 ? argv[1] : "graphs/hello_graph.yaml";

  rkavp::NodeRegistry registry;
  rkavp::RegisterBuiltinNodes(&registry);

  rkavp::GraphConfig config;
  rkavp::Status status = rkavp::YamlGraphLoader::LoadFile(graph_path, &config);
  if (!status.ok()) return Fail(status);

  rkavp::Graph graph(std::move(config), &registry);
  status = graph.Validate();
  if (!status.ok()) return Fail(status);

  std::mutex mutex;
  std::condition_variable output_ready;
  std::string result;
  rkavp::GraphRunner runner(std::move(graph), &registry);
  status = runner.ObserveOutput("output", [&](const rkavp::Packet& packet) {
    if (!packet.Is<std::string>()) return;
    {
      std::lock_guard<std::mutex> lock(mutex);
      result = packet.Get<std::string>();
    }
    output_ready.notify_one();
  });
  if (!status.ok()) return Fail(status);

  status = runner.Start();
  if (!status.ok()) return Fail(status);
  status = runner.AddPacket("input", rkavp::Packet::Make(std::string("hello RK-AVP"),
                                                         rkavp::Timestamp::FromMicroseconds(0)));
  if (status.ok()) status = runner.CloseInputStream("input");
  if (status.ok()) status = runner.WaitUntilDone(1000);
  if (!status.ok()) {
    runner.Stop();
    return Fail(status);
  }

  std::unique_lock<std::mutex> lock(mutex);
  const bool observed =
      output_ready.wait_for(lock, std::chrono::seconds(1), [&] { return !result.empty(); });
  lock.unlock();
  runner.Stop();
  if (!observed) return Fail(rkavp::Status::Unavailable("graph produced no output"));

  std::cout << result << '\n';
  return 0;
}
