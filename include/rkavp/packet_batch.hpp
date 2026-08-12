#pragma once

#include <cstddef>
#include <string>
#include <utility>
#include <vector>

#include "rkavp/packet.hpp"

namespace rkavp {

struct BatchItem {
  std::string source_id;
  Packet packet;
  std::size_t batch_index = 0;
};

class PacketBatch {
 public:
  PacketBatch() = default;
  explicit PacketBatch(std::vector<BatchItem> items, Timestamp timestamp = Timestamp::Unset())
      : items_(std::move(items)), timestamp_(timestamp) {}

  const std::vector<BatchItem>& items() const { return items_; }
  Timestamp timestamp() const { return timestamp_; }
  bool empty() const { return items_.empty(); }
  std::size_t size() const { return items_.size(); }

 private:
  std::vector<BatchItem> items_;
  Timestamp timestamp_ = Timestamp::Unset();
};

inline Packet MakePacketBatch(std::vector<BatchItem> items,
                              Timestamp timestamp = Timestamp::Unset()) {
  Packet lifetime;
  for (const auto& item : items) lifetime.InheritLifetimeTokens(item.packet);
  Packet packet = Packet::Make(PacketBatch(std::move(items), timestamp), timestamp);
  packet.InheritLifetimeTokens(lifetime);
  return packet;
}

}  // namespace rkavp
