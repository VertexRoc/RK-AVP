#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <string>
#include <thread>

#include "rkavp/core.hpp"

namespace rkavp {
namespace {

int EnduranceSeconds() {
  const char* value = std::getenv("RKAVP_ENDURANCE_SECONDS");
  if (value == nullptr) return 1;
  const int seconds = std::atoi(value);
  return seconds > 0 ? seconds : 1;
}

TEST(FrameworkEnduranceTest, SustainsBoundedGraphTrafficAndStops) {
  NodeRegistry registry;
  RegisterBuiltinNodes(&registry);
  GraphConfig config;
  config.name = "endurance";
  config.executors = {{"media", 2, 256, 0}};
  config.nodes = {{"pass", "Passthrough", "media", {}}};
  config.inputs = {{"input", "pass", "in"}};
  config.outputs = {{"output", "pass", "out"}};
  Graph graph(std::move(config), &registry);
  ASSERT_TRUE(graph.Validate().ok());

  GraphRunner runner(std::move(graph), &registry);
  std::atomic<std::uint64_t> observed{0};
  ASSERT_TRUE(runner.ObserveOutput("output", [&observed](const Packet&) { ++observed; }).ok());
  ASSERT_TRUE(runner.Start().ok());

  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(EnduranceSeconds());
  std::uint64_t submitted = 0;
  while (std::chrono::steady_clock::now() < deadline) {
    for (int batch = 0; batch < 100; ++batch) {
      const Timestamp timestamp = Timestamp::FromMicroseconds(static_cast<std::int64_t>(submitted));
      ASSERT_TRUE(runner.AddPacket("input", Packet::Make(submitted, timestamp)).ok());
      ++submitted;
    }
    ASSERT_TRUE(runner.WaitUntilIdle(1000).ok());
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  ASSERT_TRUE(runner.CloseInputStream("input").ok());
  ASSERT_TRUE(runner.WaitUntilDone(2000).ok());
  runner.Stop();
  EXPECT_EQ(observed.load(), submitted);
  EXPECT_TRUE(runner.last_error().ok());
}

}  // namespace
}  // namespace rkavp
