#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <future>
#include <stdexcept>
#include <thread>

#include "rkavp/bounded_queue.hpp"
#include "rkavp/scheduler.hpp"

namespace rkavp {
namespace {

TEST(QueueTest, AppliesDropPolicies) {
  BoundedQueue<int> oldest(2, QueuePolicy::kDropOldest);
  bool dropped = false;
  EXPECT_TRUE(oldest.Push(1));
  EXPECT_TRUE(oldest.Push(2));
  EXPECT_TRUE(oldest.Push(3, &dropped));
  EXPECT_TRUE(dropped);
  int value = 0;
  EXPECT_TRUE(oldest.WaitPop(&value));
  EXPECT_EQ(value, 2);

  BoundedQueue<int> newest(1, QueuePolicy::kDropNewest);
  EXPECT_TRUE(newest.Push(4));
  EXPECT_FALSE(newest.Push(5, &dropped));
  EXPECT_TRUE(dropped);
  EXPECT_EQ(newest.size(), 1U);
}

TEST(QueueTest, CloseWakesBlockedProducerAndConsumer) {
  BoundedQueue<int> full(1, QueuePolicy::kBlock);
  ASSERT_TRUE(full.Push(1));
  auto producer = std::async(std::launch::async, [&] { return full.Push(2); });
  full.Close();
  EXPECT_FALSE(producer.get());

  BoundedQueue<int> empty(1, QueuePolicy::kBlock);
  auto consumer = std::async(std::launch::async, [&] {
    int value = 0;
    return empty.WaitPop(&value);
  });
  empty.Close();
  EXPECT_FALSE(consumer.get());
}

TEST(QueueTest, ControlPacketReplacesDataWhenDropNewestQueueIsFull) {
  BoundedQueue<int> queue(1, QueuePolicy::kDropNewest);
  bool dropped = false;
  ASSERT_TRUE(queue.Push(1, &dropped));
  EXPECT_FALSE(dropped);
  ASSERT_TRUE(queue.PushControl(2, &dropped));
  EXPECT_TRUE(dropped);
  int value = 0;
  ASSERT_TRUE(queue.TryPop(&value));
  EXPECT_EQ(value, 2);
}

TEST(SchedulerTest, RunsQueuedTasksAndStopsCleanly) {
  Scheduler scheduler(2);
  ASSERT_TRUE(scheduler.Start().ok());
  std::promise<void> completed;
  std::atomic<int> count{0};
  for (int i = 0; i < 4; ++i) {
    ASSERT_TRUE(scheduler
                    .Post([&] {
                      if (++count == 4) {
                        completed.set_value();
                      }
                    })
                    .ok());
  }
  EXPECT_EQ(completed.get_future().wait_for(std::chrono::seconds(1)), std::future_status::ready);
  scheduler.Stop();
  EXPECT_FALSE(scheduler.running());
  EXPECT_EQ(scheduler.Post([] {}).code(), StatusCode::kFailedPrecondition);
}

TEST(SchedulerTest, CapturesTaskExceptionsWithoutTerminatingWorkers) {
  Scheduler scheduler(1);
  ASSERT_TRUE(scheduler.Start().ok());
  std::promise<void> continued;
  ASSERT_TRUE(scheduler.Post([] { throw std::runtime_error("bad task"); }).ok());
  ASSERT_TRUE(scheduler.Post([&] { continued.set_value(); }).ok());
  EXPECT_EQ(continued.get_future().wait_for(std::chrono::seconds(1)), std::future_status::ready);
  scheduler.Stop();
  EXPECT_EQ(scheduler.last_error().code(), StatusCode::kInternal);
  EXPECT_NE(scheduler.last_error().message().find("bad task"), std::string::npos);
}

TEST(SchedulerTest, ReportsQueueStarvationUsingFakeClock) {
  auto clock = std::make_shared<FakeClock>();
  Scheduler scheduler(1, 4, "fairness", 0, clock);
  ASSERT_TRUE(scheduler.Start().ok());
  std::promise<void> entered;
  std::promise<void> release;
  auto release_future = release.get_future().share();
  ASSERT_TRUE(scheduler
                  .Post([&] {
                    entered.set_value();
                    release_future.wait();
                  })
                  .ok());
  ASSERT_EQ(entered.get_future().wait_for(std::chrono::seconds(1)), std::future_status::ready);
  ASSERT_TRUE(scheduler.Post([] {}).ok());
  clock->AdvanceMicros(100001);
  release.set_value();
  ASSERT_TRUE(scheduler.WaitIdle(1000).ok());
  EXPECT_EQ(scheduler.starvation_events(), 1U);
  EXPECT_GE(scheduler.max_wait_us(), 100001);
  scheduler.Stop();
}

TEST(SchedulerTest, GuaranteedDrainTokensSurviveQueueSaturation) {
  Scheduler scheduler(1, 1, "bounded");
  ASSERT_TRUE(scheduler.Start().ok());
  std::promise<void> entered;
  std::promise<void> release;
  auto release_future = release.get_future().share();
  ASSERT_TRUE(scheduler
                  .Post([&] {
                    entered.set_value();
                    release_future.wait();
                  })
                  .ok());
  ASSERT_EQ(entered.get_future().wait_for(std::chrono::seconds(1)), std::future_status::ready);
  ASSERT_TRUE(scheduler.Post([] {}).ok());
  EXPECT_EQ(scheduler.Post([] {}).code(), StatusCode::kUnavailable);
  EXPECT_TRUE(scheduler.PostGuaranteed([] {}).ok());
  release.set_value();
  EXPECT_TRUE(scheduler.WaitIdle(1000).ok());
  scheduler.Stop();
}

}  // namespace
}  // namespace rkavp
