#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <string>
#include <vector>

#include "rkavp/core.hpp"

namespace rkavp {
namespace {

bool Contains(const std::vector<std::string>& values, const std::string& value) {
  return std::find(values.begin(), values.end(), value) != values.end();
}

TEST(HardwareSmokeTest, ReportsExpectedRockchipDevices) {
  const PlatformCapabilities capabilities = ProbePlatformCapabilities();
  EXPECT_EQ(capabilities.soc, RKAVP_TARGET_SOC);
  EXPECT_TRUE(capabilities.has_video) << "no /dev/video* capture device";
  EXPECT_GE(capabilities.video_device_count, 2U)
      << "hardware tests require at least two camera video nodes";
  EXPECT_TRUE(capabilities.has_drm) << "no DRM render node";
  EXPECT_TRUE(capabilities.has_rga) << "no RGA device";
  EXPECT_TRUE(capabilities.has_npu) << "no RKNN/NPU device";
  EXPECT_TRUE(capabilities.has_audio) << "no ALSA control device";
}

TEST(HardwareSmokeTest, LoadsInstalledBackendPlugins) {
  NodeRegistry registry;
  RegisterBuiltinNodes(&registry);
  PluginManager plugins(&registry);
  ASSERT_TRUE(plugins.Load(RKAVP_ROCKCHIP_PLUGIN_PATH).ok());
#ifdef RKAVP_AUDIO_PLUGIN_PATH
  ASSERT_TRUE(plugins.Load(RKAVP_AUDIO_PLUGIN_PATH).ok());
#endif
#ifdef RKAVP_STREAMING_PLUGIN_PATH
  ASSERT_TRUE(plugins.Load(RKAVP_STREAMING_PLUGIN_PATH).ok());
#endif
  const std::vector<std::string> types = registry.Types();
  EXPECT_TRUE(Contains(types, "V4l2Source"));
  EXPECT_TRUE(Contains(types, "MppDecoder"));
  EXPECT_TRUE(Contains(types, "MppEncoder"));
  EXPECT_TRUE(Contains(types, "RgaTransform"));
  EXPECT_TRUE(Contains(types, "RknnInference"));
#ifdef RKAVP_AUDIO_PLUGIN_PATH
  EXPECT_TRUE(Contains(types, "AlsaCapture"));
#endif
#ifdef RKAVP_STREAMING_PLUGIN_PATH
  EXPECT_TRUE(Contains(types, "StreamingInput"));
  EXPECT_TRUE(Contains(types, "StreamingOutput"));
#endif
}

TEST(HardwarePipelineTest, CapturesFromTwoConfiguredCameras) {
  const char* first_device = std::getenv("RKAVP_TEST_CAMERA_0");
  const char* second_device = std::getenv("RKAVP_TEST_CAMERA_1");
  ASSERT_NE(first_device, nullptr)
      << "set RKAVP_TEST_CAMERA_0 to a capture-capable /dev/video* node";
  ASSERT_NE(second_device, nullptr)
      << "set RKAVP_TEST_CAMERA_1 to a different capture-capable /dev/video* node";
  ASSERT_STRNE(first_device, second_device);

  NodeRegistry registry;
  RegisterBuiltinNodes(&registry);
  PluginManager plugins(&registry);
  ASSERT_TRUE(plugins.Load(RKAVP_ROCKCHIP_PLUGIN_PATH).ok());

  GraphConfig config;
  config.version = 2;
  config.name = "dual-camera-hardware";
  auto camera = [](std::string id, const char* device, std::string executor) {
    NodeConfig node;
    node.id = std::move(id);
    node.type = "V4l2Source";
    node.executor = std::move(executor);
    node.options = {{"device", ConfigValue(device)}, {"width", ConfigValue(640)},
                    {"height", ConfigValue(480)},    {"fps", ConfigValue(30)},
                    {"format", ConfigValue("yuyv")}, {"io_mode", ConfigValue("export_dmabuf")}};
    return node;
  };
  config.executors = {{"camera0", 1, 32, 0}, {"camera1", 1, 32, 0}};
  config.nodes = {camera("camera0", first_device, "camera0"),
                  camera("camera1", second_device, "camera1")};
  config.outputs = {{"camera0", "camera0", "packet"}, {"camera1", "camera1", "packet"}};

  GraphRunner runner(Graph(std::move(config), &registry), &registry);
  std::atomic<int> first_frames{0};
  std::atomic<int> second_frames{0};
  ObserverHandle first_observer;
  ObserverHandle second_observer;
  ASSERT_TRUE(runner
                  .ObserveOutput(
                      "camera0",
                      [&first_frames](const Packet& packet) {
                        if (packet.event() == ControlEvent::kNone) ++first_frames;
                      },
                      {}, &first_observer)
                  .ok());
  ASSERT_TRUE(runner
                  .ObserveOutput(
                      "camera1",
                      [&second_frames](const Packet& packet) {
                        if (packet.event() == ControlEvent::kNone) ++second_frames;
                      },
                      {}, &second_observer)
                  .ok());
  ASSERT_TRUE(runner.Start().ok());
  ASSERT_TRUE(runner.WaitForObservedOutput(first_observer, 5000).ok());
  ASSERT_TRUE(runner.WaitForObservedOutput(second_observer, 5000).ok());
  runner.Stop();
  EXPECT_GT(first_frames.load(), 0);
  EXPECT_GT(second_frames.load(), 0);
}

}  // namespace
}  // namespace rkavp
