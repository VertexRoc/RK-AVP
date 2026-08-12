#include <dirent.h>
#include <fcntl.h>
#include <gtest/gtest.h>
#include <unistd.h>

#include <atomic>
#include <memory>
#include <stdexcept>
#include <string>

#include "rkavp/core.hpp"

namespace rkavp {
namespace {

std::size_t CountOpenFileDescriptors() {
  DIR* directory = ::opendir("/proc/self/fd");
  if (directory == nullptr) {
    return 0;
  }
  std::size_t count = 0;
  while (::readdir(directory) != nullptr) {
    ++count;
  }
  ::closedir(directory);
  return count >= 2 ? count - 2 : 0;
}

TEST(TimestampTest, ConvertsTimeBaseAndPreservesSpecialValues) {
  EXPECT_EQ(Timestamp::FromTicks(90, {1, 90000}).microseconds(), 1000);
  EXPECT_TRUE(Timestamp::Unset().is_unset());
  EXPECT_TRUE(Timestamp::Done().is_done());
  EXPECT_LT(Timestamp::FromMicroseconds(1), Timestamp::FromMicroseconds(2));
  EXPECT_THROW(Timestamp::FromTicks(1, {0, 1}), std::invalid_argument);
  EXPECT_THROW(Timestamp::FromTicks(Timestamp::kDoneValue - 1, {1000000, 1}), std::overflow_error);
}

TEST(PacketTest, SharesPayloadAndCarriesTypedMetadata) {
  auto buffer = std::make_shared<HostBuffer>(32);
  Packet original = Packet::Make<BufferPtr>(buffer, Timestamp::FromMicroseconds(7));
  original.mutable_metadata().Set("source_id", std::string("camera0"));
  original.mutable_metadata().Set("frame_id", std::int64_t{42});

  Packet copy = original;
  EXPECT_TRUE(copy.Is<BufferPtr>());
  EXPECT_EQ(copy.Get<BufferPtr>().get(), buffer.get());
  EXPECT_EQ(copy.timestamp().microseconds(), 7);
  EXPECT_EQ(copy.metadata().Get<std::string>("source_id"), "camera0");
  EXPECT_EQ(copy.metadata().Get<std::int64_t>("frame_id"), 42);
  EXPECT_THROW(copy.Get<int>(), std::bad_cast);
}

TEST(PacketBatchTest, RetainsItemLifetimeTokensUntilBatchRelease) {
  struct LifetimeProbe {
    explicit LifetimeProbe(std::atomic<int>* destroyed) : destroyed(destroyed) {}
    ~LifetimeProbe() { destroyed->fetch_add(1); }
    std::atomic<int>* destroyed;
  };

  std::atomic<int> destroyed{0};
  Packet item = Packet::Make<int>(7, Timestamp::FromMicroseconds(9));
  item.AddLifetimeToken(std::make_shared<LifetimeProbe>(&destroyed));
  Packet batch = MakePacketBatch({BatchItem{"camera0", item, 0}}, item.timestamp());
  item = Packet{};

  EXPECT_EQ(destroyed.load(), 0);
  ASSERT_TRUE(batch.Is<PacketBatch>());
  EXPECT_EQ(batch.Get<PacketBatch>().items().front().packet.Get<int>(), 7);
  batch = Packet{};
  EXPECT_EQ(destroyed.load(), 1);
}

TEST(DmaBufferTest, DuplicateOwnsAnIndependentFileDescriptor) {
  const int original = ::open("/dev/null", O_RDONLY);
  ASSERT_GE(original, 0);
  int duplicate = -1;
  {
    auto buffer = DmaBuffer::Duplicate(original, 4096);
    duplicate = buffer->fd();
    EXPECT_NE(duplicate, original);
    EXPECT_EQ(buffer->memory_type(), MemoryType::kDmaBuf);
    EXPECT_EQ(buffer->size(), 4096U);
    EXPECT_NE(::fcntl(duplicate, F_GETFD), -1);
  }
  EXPECT_EQ(::fcntl(duplicate, F_GETFD), -1);
  EXPECT_NE(::fcntl(original, F_GETFD), -1);
  ::close(original);
}

TEST(DmaBufferTest, DuplicateOwnsEveryDistinctPlaneFileDescriptor) {
  const int first = ::open("/dev/null", O_RDONLY);
  const int second = ::open("/dev/null", O_RDONLY);
  ASSERT_GE(first, 0);
  ASSERT_GE(second, 0);
  int owned_first = -1;
  int owned_second = -1;
  {
    auto buffer = DmaBuffer::Duplicate(first, 4096,
                                       {{first, 0, 2048, 64, 1, 0}, {second, 0, 2048, 64, 1, 0}});
    ASSERT_EQ(buffer->planes().size(), 2U);
    owned_first = buffer->planes()[0].fd;
    owned_second = buffer->planes()[1].fd;
    EXPECT_NE(owned_first, first);
    EXPECT_NE(owned_second, second);
    EXPECT_NE(owned_first, owned_second);
    EXPECT_NE(::fcntl(owned_first, F_GETFD), -1);
    EXPECT_NE(::fcntl(owned_second, F_GETFD), -1);
    EXPECT_NE(::fcntl(owned_first, F_GETFD) & FD_CLOEXEC, 0);
    EXPECT_NE(::fcntl(owned_second, F_GETFD) & FD_CLOEXEC, 0);
  }
  EXPECT_EQ(::fcntl(owned_first, F_GETFD), -1);
  EXPECT_EQ(::fcntl(owned_second, F_GETFD), -1);
  EXPECT_NE(::fcntl(first, F_GETFD), -1);
  EXPECT_NE(::fcntl(second, F_GETFD), -1);
  ::close(first);
  ::close(second);
}

TEST(DmaBufferTest, ReturnsFileDescriptorCountToBaseline) {
  const std::size_t before = CountOpenFileDescriptors();
  {
    const int original = ::open("/dev/null", O_RDONLY);
    ASSERT_GE(original, 0);
    auto buffer = DmaBuffer::Duplicate(original, 128);
    Packet packet = Packet::Make<BufferPtr>(buffer);
    Packet queue_copy = packet;
    EXPECT_EQ(queue_copy.Get<BufferPtr>().get(), buffer.get());
    ::close(original);
  }
  EXPECT_EQ(CountOpenFileDescriptors(), before);
}

TEST(BufferPoolTest, EnforcesCapacityAndReusesReleasedBuffers) {
  auto allocator = std::make_shared<HostBufferAllocator>();
  BufferPool pool(allocator, 256, 2);
  BufferPtr first;
  BufferPtr second;
  BufferPtr exhausted;
  ASSERT_TRUE(pool.Acquire(&first, 0).ok());
  ASSERT_TRUE(pool.Acquire(&second, 0).ok());
  EXPECT_EQ(pool.Acquire(&exhausted, 0).code(), StatusCode::kUnavailable);
  Buffer* first_address = first.get();
  ASSERT_TRUE(pool.Release(std::move(first)).ok());
  ASSERT_TRUE(pool.Acquire(&exhausted, 0).ok());
  EXPECT_EQ(exhausted.get(), first_address);
  pool.Close();
  EXPECT_EQ(pool.Acquire(&first, 0).code(), StatusCode::kCancelled);
}

TEST(HardwareContextServiceTest, SharesPoolsContextsImportsAndFences) {
  class ReadyFence final : public Fence {
   public:
    Status Wait(std::int64_t timeout_ms) override {
      waited = timeout_ms;
      return Status::Ok();
    }
    std::int64_t waited = -1;
  };
  HardwareContextService service;
  std::shared_ptr<BufferPool> first_pool;
  std::shared_ptr<BufferPool> second_pool;
  ASSERT_TRUE(service.GetOrCreatePool("video", 128, 2, &first_pool).ok());
  ASSERT_TRUE(service.GetOrCreatePool("video", 128, 2, &second_pool).ok());
  EXPECT_EQ(first_pool, second_pool);

  auto context = std::make_shared<int>(7);
  ASSERT_TRUE(service.SetBackendContext("rknn", context).ok());
  EXPECT_EQ(service.GetBackendContext<int>("rknn"), context);
  auto imported = std::make_shared<std::string>("rga-handle");
  ASSERT_TRUE(service.CacheImport("rga", 10, imported).ok());
  EXPECT_EQ(service.FindImport<std::string>("rga", 10), imported);
  EXPECT_EQ(service.ImportCount("rga", 10), 1U);

  BufferPtr buffer = std::make_shared<HostBuffer>(16);
  auto fence = std::make_shared<ReadyFence>();
  buffer->set_fence(fence);
  EXPECT_TRUE(service.WaitFence(buffer, 25).ok());
  EXPECT_EQ(fence->waited, 25);
  EXPECT_TRUE(service.Sync(buffer, SyncDirection::kCpuToDevice).ok());
}

TEST(MediaCapsTest, ValidatesVideoAudioTensorAndMemoryCompatibility) {
  MediaCaps nv12{MediaKind::kVideo, "nv12", 1920, 1080, 0, 0, {MemoryType::kDmaBuf}};
  MediaCaps any_dma{MediaKind::kVideo, "", 0, 0, 0, 0, {MemoryType::kDmaBuf, MemoryType::kMpp}};
  MediaCaps rgb_host{MediaKind::kVideo, "rgb", 1920, 1080, 0, 0, {MemoryType::kHost}};
  MediaCaps audio{MediaKind::kAudio, "s16le", 0, 0, 16000, 1, {MemoryType::kHost}};
  MediaCaps tensor{MediaKind::kTensor, "nchw", 0, 0, 0, 0, {MemoryType::kRknn}};

  EXPECT_TRUE(nv12.CompatibleWith(any_dma));
  EXPECT_FALSE(nv12.CompatibleWith(rgb_host));
  EXPECT_FALSE(audio.CompatibleWith(tensor));
  EXPECT_TRUE(MediaCaps::Any().CompatibleWith(tensor));
}

TEST(MetadataTest, KeepsFrameCorrelationFields) {
  MetadataSet metadata;
  metadata.Set("source_id", std::string("mic0"));
  metadata.Set("pts_us", std::int64_t{1234});
  metadata.Set("rotation", 90.0);
  metadata.Set("key_frame", true);
  EXPECT_EQ(metadata.Get<std::string>("source_id"), "mic0");
  EXPECT_EQ(metadata.Get<std::int64_t>("pts_us"), 1234);
  EXPECT_EQ(metadata.Get<double>("rotation"), 90.0);
  EXPECT_EQ(metadata.Get<bool>("key_frame"), true);
  EXPECT_FALSE(metadata.Get<std::string>("missing").has_value());
}

TEST(ConfigValueTest, ConvertsScalarAndStructuredValuesToDiagnosticStrings) {
  EXPECT_EQ(ConfigValue().ToString(), "");
  EXPECT_EQ(ConfigValue(true).ToString(), "true");
  EXPECT_EQ(ConfigValue(false).ToString(), "false");
  EXPECT_EQ(ConfigValue(std::int64_t{42}).ToString(), "42");
  EXPECT_EQ(ConfigValue(1.5).ToString(), "1.5");
  EXPECT_EQ(ConfigValue("camera").ToString(), "camera");
  EXPECT_EQ(ConfigValue(ConfigValue::Array{ConfigValue(1)}).ToString(), "[array]");
  EXPECT_EQ(ConfigValue(ConfigValue::Object{{"enabled", ConfigValue(true)}}).ToString(),
            "{object}");
}

TEST(MetricsTest, StoresDefaultsAndExportsCountersAndGauges) {
  MetricsRegistry metrics;
  EXPECT_EQ(metrics.Counter("missing"), 0U);
  EXPECT_DOUBLE_EQ(metrics.Gauge("missing"), 0.0);
  metrics.Increment("frames");
  metrics.Increment("frames", 2);
  metrics.SetGauge("queue_depth", 1.5);
  EXPECT_EQ(metrics.Counter("frames"), 3U);
  EXPECT_DOUBLE_EQ(metrics.Gauge("queue_depth"), 1.5);
  const std::string text = metrics.ExportText();
  EXPECT_NE(text.find("frames 3\n"), std::string::npos);
  EXPECT_NE(text.find("queue_depth 1.5\n"), std::string::npos);
}

TEST(TraceTest, WrapsInChronologicalOrderAndEscapesChromeJson) {
  TraceBuffer trace(2);
  trace.Add({"first", "node", "old", "default", 1, 2, 3});
  trace.Add({"second", "node", "middle", "default", 4, 5, 6});
  trace.Add({"third\"", "node\\category", "new", "worker", 7, 8, 9});
  const auto events = trace.Snapshot();
  ASSERT_EQ(events.size(), 2U);
  EXPECT_EQ(events[0].name, "second");
  EXPECT_EQ(events[1].name, "third\"");
  const std::string json = trace.ToChromeTraceJson();
  EXPECT_EQ(json.find("first"), std::string::npos);
  EXPECT_NE(json.find("third\\\""), std::string::npos);
  EXPECT_NE(json.find("node\\\\category"), std::string::npos);

  TraceBuffer minimum_capacity(0);
  minimum_capacity.Add({"only", "node", "", "", 0, 0, 0});
  EXPECT_EQ(minimum_capacity.Snapshot().size(), 1U);
}

}  // namespace
}  // namespace rkavp
