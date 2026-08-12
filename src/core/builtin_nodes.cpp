#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <mutex>
#include <thread>

#include "rkavp/bounded_queue.hpp"
#include "rkavp/clock.hpp"
#include "rkavp/media_types.hpp"
#include "rkavp/node_registry.hpp"
#include "rkavp/packet_batch.hpp"

namespace rkavp {
namespace {

class PassthroughNode final : public Node {
 public:
  NodeContract Contract() const override {
    return {{{"in", MediaCaps::Any(), true}},
            {{"out", MediaCaps::Any(), true}},
            {},
            InputPolicy::kAny,
            {}};
  }

 protected:
  Status OnProcess(NodeContext& context) override {
    const Packet* packet = context.Input("in");
    return packet == nullptr ? Status::Invalid("Passthrough requires input")
                             : context.Emit("out", *packet);
  }
};

class NullSinkNode final : public Node {
 public:
  NodeContract Contract() const override {
    return {{{"in", MediaCaps::Any(), true}}, {}, {}, InputPolicy::kAny, {}};
  }

 protected:
  Status OnProcess(NodeContext&) override { return Status::Ok(); }
};

QueuePolicy QueuePolicyOption(const NodeOptions& options, const std::string& name,
                              QueuePolicy fallback) {
  const ConfigValue* value = FindOption(options, name);
  if (value == nullptr || !value->Is<std::string>()) return fallback;
  if (value->As<std::string>() == "block") return QueuePolicy::kBlock;
  if (value->As<std::string>() == "drop_newest") return QueuePolicy::kDropNewest;
  return QueuePolicy::kDropOldest;
}

class FlowLimiterNode final : public Node {
 public:
  NodeContract Contract() const override {
    return {{{"in", MediaCaps::Any(), true}, {"finished", MediaCaps::Any(), false}},
            {{"out", MediaCaps::Any(), true}},
            {},
            InputPolicy::kAny,
            {}};
  }

 protected:
  Status OnConfigure(const NodeOptions& options) override {
    std::int64_t value = 0;
    if (FindOption(options, "max_in_flight") != nullptr) {
      Status status = GetIntegerOption(options, "max_in_flight", &value);
      if (!status.ok() || value <= 0) return Status::Invalid("max_in_flight must be positive");
      max_in_flight_ = static_cast<std::size_t>(value);
    }
    if (FindOption(options, "queue_capacity") != nullptr) {
      Status status = GetIntegerOption(options, "queue_capacity", &value);
      if (!status.ok() || value < 0) return Status::Invalid("queue_capacity must be non-negative");
      queue_capacity_ = static_cast<std::size_t>(value);
    }
    policy_ = QueuePolicyOption(options, "queue_policy", QueuePolicy::kDropOldest);
    return Status::Ok();
  }

  Status OnProcess(NodeContext& context) override {
    if (context.Input("finished") != nullptr) {
      if (in_flight_ != 0) --in_flight_;
      if (!pending_.empty() && in_flight_ < max_in_flight_) {
        Packet packet = std::move(pending_.front());
        pending_.pop_front();
        ++in_flight_;
        return context.Emit("out", std::move(packet));
      }
      return Status::Ok();
    }
    const Packet* packet = context.Input("in");
    if (packet == nullptr) return Status::Invalid("FlowLimiter requires in or finished input");
    if (packet->event() != ControlEvent::kNone) return context.Emit("out", *packet);
    if (in_flight_ < max_in_flight_) {
      ++in_flight_;
      return context.Emit("out", *packet);
    }
    if (queue_capacity_ == 0) {
      context.metrics()->Increment("flow_limiter.dropped");
      return Status::Ok();
    }
    if (pending_.size() >= queue_capacity_) {
      context.metrics()->Increment("flow_limiter.dropped");
      if (policy_ == QueuePolicy::kDropNewest || policy_ == QueuePolicy::kBlock)
        return Status::Ok();
      pending_.pop_front();
    }
    pending_.push_back(*packet);
    return Status::Ok();
  }

 private:
  std::size_t max_in_flight_ = 1;
  std::size_t queue_capacity_ = 1;
  QueuePolicy policy_ = QueuePolicy::kDropOldest;
  std::size_t in_flight_ = 0;
  std::deque<Packet> pending_;
};

class AdaptiveBatchNode final : public Node {
 public:
  ~AdaptiveBatchNode() override { StopWorker(); }
  NodeContract Contract() const override {
    return {{{"in", MediaCaps::Any(), true}, {"flush", MediaCaps::Any(), false}},
            {{"batch", MediaCaps::Any(), true}},
            {},
            InputPolicy::kAny,
            {}};
  }

 protected:
  Status OnConfigure(const NodeOptions& options) override {
    std::int64_t value = 0;
    if (FindOption(options, "max_batch_size") != nullptr) {
      Status status = GetIntegerOption(options, "max_batch_size", &value);
      if (!status.ok() || value <= 0) return Status::Invalid("max_batch_size must be positive");
      max_batch_size_ = static_cast<std::size_t>(value);
    }
    if (FindOption(options, "timeout_us") != nullptr) {
      Status status = GetIntegerOption(options, "timeout_us", &value);
      if (!status.ok() || value < 0) return Status::Invalid("timeout_us must be non-negative");
      timeout_us_ = value;
    }
    if (FindOption(options, "max_per_source") != nullptr) {
      Status status = GetIntegerOption(options, "max_per_source", &value);
      if (!status.ok() || value <= 0) return Status::Invalid("max_per_source must be positive");
      max_per_source_ = static_cast<std::size_t>(value);
    }
    if (FindOption(options, "allow_partial") != nullptr) {
      Status status = GetBoolOption(options, "allow_partial", &allow_partial_);
      if (!status.ok()) return status;
    }
    if (FindOption(options, "mode") != nullptr) {
      std::string mode;
      Status status = GetStringOption(options, "mode", &mode);
      if (!status.ok()) return status;
      if (mode == "low_latency")
        allow_partial_ = true;
      else if (mode == "high_utilization")
        allow_partial_ = false;
      else
        return Status::Invalid("batch mode must be low_latency or high_utilization");
    }
    return Status::Ok();
  }

  Status OnStart(NodeContext& context) override {
    context_ = &context;
    clock_ = context.Service<IClock>("rkavp.clock");
    if (!clock_) clock_ = std::make_shared<SteadyClock>();
    stopped_ = false;
    if (timeout_us_ > 0 && allow_partial_) worker_ = std::thread([this] { TimerLoop(); });
    return Status::Ok();
  }

  Status OnProcess(NodeContext& context) override {
    if (context.Input("flush") != nullptr) return Flush(context);
    const Packet* packet = context.Input("in");
    if (packet == nullptr) return Status::Invalid("AdaptiveBatch requires input");
    if (packet->event() == ControlEvent::kEndOfStream) {
      Status status = Flush(context);
      return status.ok() ? context.Emit("batch", *packet) : status;
    }
    if (packet->event() != ControlEvent::kNone) return context.Emit("batch", *packet);
    bool expired = false;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      expired = !pending_.empty() && timeout_us_ > 0 &&
                clock_->NowMicros() - first_packet_time_us_ >= timeout_us_;
    }
    if (expired) {
      Status status = Flush(context);
      if (!status.ok()) return status;
    }
    bool flush = false;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      std::string source_id = packet->metadata().Get<std::string>("source_id").value_or("default");
      const auto count =
          std::count_if(pending_.begin(), pending_.end(),
                        [&](const BatchItem& item) { return item.source_id == source_id; });
      if (static_cast<std::size_t>(count) >= max_per_source_) {
        context.metrics()->Increment("batch.source_limited");
        return Status::Ok();
      }
      pending_.push_back({std::move(source_id), *packet, pending_.size()});
      if (pending_.size() == 1) {
        first_packet_time_ = std::chrono::steady_clock::now();
        first_packet_time_us_ = clock_->NowMicros();
      }
      flush = pending_.size() >= max_batch_size_;
      condition_.notify_all();
    }
    return flush ? Flush(context) : Status::Ok();
  }

  Status OnStop() override {
    StopWorker();
    return Status::Ok();
  }

 private:
  Status Flush(NodeContext& context) {
    std::vector<BatchItem> items;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      if (pending_.empty()) return Status::Ok();
      std::vector<std::string> source_order;
      for (const auto& item : pending_) {
        if (std::find(source_order.begin(), source_order.end(), item.source_id) ==
            source_order.end()) {
          source_order.push_back(item.source_id);
        }
      }
      while (!pending_.empty()) {
        for (const auto& source : source_order) {
          const auto item = std::find_if(
              pending_.begin(), pending_.end(),
              [&](const BatchItem& candidate) { return candidate.source_id == source; });
          if (item == pending_.end()) continue;
          items.push_back(std::move(*item));
          pending_.erase(item);
        }
      }
    }
    for (std::size_t i = 0; i < items.size(); ++i) items[i].batch_index = i;
    Timestamp timestamp = items.front().packet.timestamp();
    context.metrics()->Increment("batch.emitted");
    context.metrics()->SetGauge("batch.last_size", static_cast<double>(items.size()));
    return context.Emit("batch", MakePacketBatch(std::move(items), timestamp));
  }

  void TimerLoop() {
    std::unique_lock<std::mutex> lock(mutex_);
    while (!stopped_) {
      condition_.wait(lock, [&] { return stopped_ || !pending_.empty(); });
      if (stopped_) return;
      const auto deadline = first_packet_time_ + std::chrono::microseconds(timeout_us_);
      if (condition_.wait_until(lock, deadline, [&] { return stopped_ || pending_.empty(); }))
        continue;
      NodeContext* context = context_;
      lock.unlock();
      if (context != nullptr && !context->cancelled()) (void)Flush(*context);
      lock.lock();
    }
  }

  void StopWorker() {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      stopped_ = true;
      condition_.notify_all();
    }
    if (worker_.joinable()) worker_.join();
  }

  std::size_t max_batch_size_ = 4;
  std::size_t max_per_source_ = 1;
  std::int64_t timeout_us_ = 10000;
  bool allow_partial_ = true;
  NodeContext* context_ = nullptr;
  std::shared_ptr<IClock> clock_;
  std::mutex mutex_;
  std::condition_variable condition_;
  std::vector<BatchItem> pending_;
  std::chrono::steady_clock::time_point first_packet_time_;
  std::int64_t first_packet_time_us_ = 0;
  bool stopped_ = true;
  std::thread worker_;
};

class StreamDemuxNode final : public Node {
 public:
  NodeContract Contract() const override {
    return {{{"batch", MediaCaps::Any(), true}},
            {{"out", MediaCaps::Any(), true}},
            {},
            InputPolicy::kAny,
            {}};
  }

 protected:
  Status OnProcess(NodeContext& context) override {
    const Packet* packet = context.Input("batch");
    if (packet == nullptr) return Status::Invalid("StreamDemux requires batch input");
    if (packet->event() != ControlEvent::kNone) return context.Emit("out", *packet);
    if (!packet->Is<PacketBatch>()) return Status::Invalid("StreamDemux expects PacketBatch");
    for (const auto& item : packet->Get<PacketBatch>().items()) {
      Status status = context.Emit("out", item.packet);
      if (!status.ok()) return status;
    }
    return Status::Ok();
  }
};

class EncodedPacketRingBufferNode final : public Node {
 public:
  NodeContract Contract() const override {
    return {{{"packet", {MediaKind::kEncodedVideo, "", 0, 0, 0, 0, {}, {}}, true},
             {"trigger", MediaCaps::Any(), false}},
            {{"clip", MediaCaps::Any(), true}},
            {},
            InputPolicy::kAny,
            {}};
  }

 protected:
  Status OnConfigure(const NodeOptions& options) override {
    std::int64_t value = 0;
    if (FindOption(options, "capacity") != nullptr) {
      Status status = GetIntegerOption(options, "capacity", &value);
      if (!status.ok() || value <= 0) return Status::Invalid("capacity must be positive");
      capacity_ = static_cast<std::size_t>(value);
    }
    if (FindOption(options, "post_packets") != nullptr) {
      Status status = GetIntegerOption(options, "post_packets", &value);
      if (!status.ok() || value < 0) return Status::Invalid("post_packets must be non-negative");
      post_packets_ = static_cast<std::size_t>(value);
    }
    return Status::Ok();
  }
  Status OnProcess(NodeContext& context) override {
    if (context.Input("trigger") != nullptr) {
      clip_.assign(packets_.begin(), packets_.end());
      remaining_post_packets_ = post_packets_;
      recording_ = post_packets_ != 0;
      return recording_ ? Status::Ok() : EmitClip(context);
    }
    const Packet* packet = context.Input("packet");
    if (packet == nullptr)
      return Status::Invalid("EncodedPacketRingBuffer requires packet or trigger");
    if (packet->event() != ControlEvent::kNone) return Status::Ok();
    if (!packet->Is<EncodedPacket>()) return Status::Invalid("ring buffer expects EncodedPacket");
    if (packets_.size() == capacity_) packets_.pop_front();
    packets_.push_back(*packet);
    if (recording_) {
      clip_.push_back(*packet);
      if (remaining_post_packets_ != 0) --remaining_post_packets_;
      if (remaining_post_packets_ == 0) {
        recording_ = false;
        return EmitClip(context);
      }
    }
    return Status::Ok();
  }

 private:
  Status EmitClip(NodeContext& context) {
    std::vector<BatchItem> items;
    items.reserve(clip_.size());
    for (std::size_t i = 0; i < clip_.size(); ++i) {
      items.push_back(
          {clip_[i].metadata().Get<std::string>("source_id").value_or("default"), clip_[i], i});
    }
    clip_.clear();
    if (items.empty()) return Status::Ok();
    Timestamp timestamp = items.front().packet.timestamp();
    return context.Emit("clip", MakePacketBatch(std::move(items), timestamp));
  }
  std::size_t capacity_ = 300;
  std::size_t post_packets_ = 0;
  std::size_t remaining_post_packets_ = 0;
  bool recording_ = false;
  std::deque<Packet> packets_;
  std::vector<Packet> clip_;
};

}  // namespace

void RegisterBuiltinNodes(NodeRegistry* registry) {
  if (registry == nullptr) {
    return;
  }
  registry->Register("Passthrough", [] { return std::make_unique<PassthroughNode>(); });
  registry->Register("NullSink", [] { return std::make_unique<NullSinkNode>(); });
  registry->Register("FlowLimiter", [] { return std::make_unique<FlowLimiterNode>(); });
  registry->Register("AdaptiveBatch", [] { return std::make_unique<AdaptiveBatchNode>(); });
  registry->Register("StreamDemux", [] { return std::make_unique<StreamDemuxNode>(); });
  registry->Register("EncodedPacketRingBuffer",
                     [] { return std::make_unique<EncodedPacketRingBufferNode>(); });
}

}  // namespace rkavp
