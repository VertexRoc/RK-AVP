#pragma once

#include <cstddef>
#include <cstdint>
#include <deque>
#include <mutex>

#include "rkavp/media_types.hpp"
#include "rkavp/status.hpp"

namespace rkavp {

class RtpTimestampMapper {
 public:
  explicit RtpTimestampMapper(std::uint32_t clock_rate);
  Status Map(std::uint32_t rtp_timestamp, Timestamp* timestamp);
  void Reset();

 private:
  std::uint32_t clock_rate_;
  bool initialized_ = false;
  std::uint32_t last_ = 0;
  std::uint64_t cycles_ = 0;
  std::uint64_t origin_ = 0;
};

class RtcpClockMapper {
 public:
  explicit RtcpClockMapper(std::uint32_t clock_rate) : mapper_(clock_rate) {}
  Status Update(std::uint32_t rtp_timestamp, std::int64_t ntp_time_us);
  Status Map(std::uint32_t rtp_timestamp, Timestamp* timestamp);
  std::int64_t drift_us() const { return drift_us_; }

 private:
  RtpTimestampMapper mapper_;
  bool anchored_ = false;
  std::int64_t ntp_anchor_us_ = 0;
  std::int64_t drift_us_ = 0;
};

class ReconnectBackoff {
 public:
  ReconnectBackoff(std::int64_t initial_delay_ms, std::int64_t max_delay_ms,
                   double multiplier = 2.0);
  std::int64_t NextDelayMs();
  void Reset();
  std::uint64_t attempts() const { return attempts_; }

 private:
  std::int64_t initial_delay_ms_;
  std::int64_t max_delay_ms_;
  double multiplier_;
  std::int64_t next_delay_ms_;
  std::uint64_t attempts_ = 0;
};

class EncodedJitterBuffer {
 public:
  EncodedJitterBuffer(std::size_t capacity, std::int64_t max_latency_us);
  Status Push(EncodedPacket packet, bool* dropped = nullptr);
  bool PopReady(Timestamp watermark, EncodedPacket* packet);
  void Close();
  std::size_t size() const;

 private:
  std::size_t capacity_;
  std::int64_t max_latency_us_;
  mutable std::mutex mutex_;
  std::deque<EncodedPacket> packets_;
  Timestamp last_output_pts_ = Timestamp::Unset();
  bool closed_ = false;
};

}  // namespace rkavp
