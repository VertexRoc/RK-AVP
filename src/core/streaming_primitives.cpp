#include "rkavp/streaming_primitives.hpp"

#include <algorithm>
#include <limits>

namespace rkavp {

RtpTimestampMapper::RtpTimestampMapper(std::uint32_t clock_rate) : clock_rate_(clock_rate) {}

Status RtpTimestampMapper::Map(std::uint32_t rtp_timestamp, Timestamp* timestamp) {
  if (timestamp == nullptr) return Status::Invalid("timestamp output is null");
  if (clock_rate_ == 0) return Status::Invalid("RTP clock rate must be positive");
  if (!initialized_) {
    initialized_ = true;
    last_ = rtp_timestamp;
    origin_ = rtp_timestamp;
  } else {
    if (rtp_timestamp < last_ &&
        last_ - rtp_timestamp > (std::numeric_limits<std::uint32_t>::max() / 2U)) {
      cycles_ += (std::uint64_t{1} << 32U);
    }
    last_ = rtp_timestamp;
  }
  const std::uint64_t extended = cycles_ + rtp_timestamp;
  const std::uint64_t ticks = extended - origin_;
  *timestamp =
      Timestamp::FromMicroseconds(static_cast<std::int64_t>((ticks * 1000000ULL) / clock_rate_));
  return Status::Ok();
}

void RtpTimestampMapper::Reset() {
  initialized_ = false;
  last_ = 0;
  cycles_ = 0;
  origin_ = 0;
}

Status RtcpClockMapper::Update(std::uint32_t rtp_timestamp, std::int64_t ntp_time_us) {
  if (anchored_) {
    Timestamp relative;
    const Status status = mapper_.Map(rtp_timestamp, &relative);
    if (!status.ok()) return status;
    drift_us_ = ntp_time_us - (ntp_anchor_us_ + relative.microseconds());
  }
  mapper_.Reset();
  Timestamp zero;
  const Status status = mapper_.Map(rtp_timestamp, &zero);
  if (!status.ok()) return status;
  ntp_anchor_us_ = ntp_time_us;
  anchored_ = true;
  return Status::Ok();
}

Status RtcpClockMapper::Map(std::uint32_t rtp_timestamp, Timestamp* timestamp) {
  if (!anchored_)
    return Status::FailedPrecondition("RTCP clock mapper has no sender-report anchor");
  Timestamp relative;
  const Status status = mapper_.Map(rtp_timestamp, &relative);
  if (!status.ok()) return status;
  *timestamp = Timestamp::FromMicroseconds(ntp_anchor_us_ + relative.microseconds());
  return Status::Ok();
}

ReconnectBackoff::ReconnectBackoff(std::int64_t initial_delay_ms, std::int64_t max_delay_ms,
                                   double multiplier)
    : initial_delay_ms_(std::max<std::int64_t>(0, initial_delay_ms)),
      max_delay_ms_(std::max(initial_delay_ms_, max_delay_ms)),
      multiplier_(std::max(1.0, multiplier)),
      next_delay_ms_(initial_delay_ms_) {}

std::int64_t ReconnectBackoff::NextDelayMs() {
  const std::int64_t result = next_delay_ms_;
  ++attempts_;
  const double scaled_delay = static_cast<double>(next_delay_ms_) * multiplier_;
  next_delay_ms_ = std::min(max_delay_ms_, static_cast<std::int64_t>(scaled_delay));
  return result;
}

void ReconnectBackoff::Reset() {
  attempts_ = 0;
  next_delay_ms_ = initial_delay_ms_;
}

EncodedJitterBuffer::EncodedJitterBuffer(std::size_t capacity, std::int64_t max_latency_us)
    : capacity_(capacity == 0 ? 1 : capacity),
      max_latency_us_(std::max<std::int64_t>(0, max_latency_us)) {}

Status EncodedJitterBuffer::Push(EncodedPacket packet, bool* dropped) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (dropped != nullptr) *dropped = false;
  if (closed_) return Status::Cancelled("jitter buffer is closed");
  if (!packet.pts.is_range_value()) return Status::Invalid("jitter packet requires PTS");
  if (last_output_pts_.is_range_value() && packet.pts <= last_output_pts_) {
    if (dropped != nullptr) *dropped = true;
    return Status::Ok();
  }
  auto position =
      std::upper_bound(packets_.begin(), packets_.end(), packet.pts,
                       [](Timestamp pts, const EncodedPacket& item) { return pts < item.pts; });
  packets_.insert(position, std::move(packet));
  if (packets_.size() > capacity_) {
    packets_.pop_front();
    if (dropped != nullptr) *dropped = true;
  }
  return Status::Ok();
}

bool EncodedJitterBuffer::PopReady(Timestamp watermark, EncodedPacket* packet) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (packet == nullptr || packets_.empty() || !watermark.is_range_value()) return false;
  const Timestamp deadline =
      Timestamp::FromMicroseconds(watermark.microseconds() - max_latency_us_);
  if (packets_.front().pts > deadline) return false;
  *packet = std::move(packets_.front());
  packets_.pop_front();
  last_output_pts_ = packet->pts;
  return true;
}

void EncodedJitterBuffer::Close() {
  std::lock_guard<std::mutex> lock(mutex_);
  closed_ = true;
  packets_.clear();
}

std::size_t EncodedJitterBuffer::size() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return packets_.size();
}

}  // namespace rkavp
