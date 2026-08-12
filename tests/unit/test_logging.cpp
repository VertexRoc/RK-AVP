#include <gtest/gtest.h>

#include <mutex>
#include <string>
#include <vector>

#include "rkavp/logging.hpp"

namespace rkavp {
namespace {

class LoggingTest : public ::testing::Test {
 protected:
  void TearDown() override {
    ResetLogSink();
    InitializeLogging();
  }
};

TEST_F(LoggingTest, FiltersBySeverityAndCapturesSourceLocation) {
  std::vector<LogRecord> records;
  SetLogSink([&](const LogRecord& record) { records.push_back(record); });
  InitializeLogging({LogLevel::kWarning, true});

  RKAVP_LOG(Info) << "hidden";
  RKAVP_LOG(Error) << "visible " << 42;

  ASSERT_EQ(records.size(), 1U);
  EXPECT_EQ(records[0].level, LogLevel::kError);
  EXPECT_EQ(records[0].message, "visible 42");
  EXPECT_NE(std::string(records[0].file).find("test_logging.cpp"), std::string::npos);
  EXPECT_GT(records[0].line, 0);
}

TEST_F(LoggingTest, PropagatesGraphNodeSourceAndFrameContext) {
  std::vector<LogRecord> records;
  SetLogSink([&](const LogRecord& record) { records.push_back(record); });
  {
    ScopedLogContext context({"camera-graph", "transform", "camera0", 99});
    RKAVP_LOG(Info) << "inference complete";
  }

  ASSERT_EQ(records.size(), 1U);
  EXPECT_EQ(records[0].context.graph, "camera-graph");
  EXPECT_EQ(records[0].context.node, "transform");
  EXPECT_EQ(records[0].context.source, "camera0");
  EXPECT_EQ(records[0].context.frame_id, 99U);
}

TEST_F(LoggingTest, ParsesConfigurationValues) {
  EXPECT_EQ(ParseLogLevel("DEBUG"), LogLevel::kDebug);
  EXPECT_EQ(ParseLogLevel("warn"), LogLevel::kWarning);
  EXPECT_EQ(ParseLogLevel("invalid", LogLevel::kError), LogLevel::kError);
}

}  // namespace
}  // namespace rkavp
