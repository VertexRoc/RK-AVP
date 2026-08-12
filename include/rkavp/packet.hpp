#pragma once

#include <memory>
#include <stdexcept>
#include <typeindex>
#include <typeinfo>
#include <utility>
#include <vector>

#include "rkavp/metadata.hpp"
#include "rkavp/timestamp.hpp"

namespace rkavp {

enum class ControlEvent {
  kNone,
  kEndOfStream,
  kTimestampBound,
  kFlush,
  kPause,
  kResume,
  kReconfigure,
  kForceKeyFrame
};

class Packet {
 public:
  Packet() : type_(typeid(void)) {}

  template <typename T>
  static Packet Make(T value, Timestamp timestamp = Timestamp::Unset()) {
    Packet packet;
    auto storage = std::make_shared<T>(std::move(value));
    packet.data_ = std::move(storage);
    packet.type_ = std::type_index(typeid(T));
    packet.timestamp_ = timestamp;
    return packet;
  }

  static Packet Event(ControlEvent event, Timestamp timestamp = Timestamp::Unset()) {
    Packet packet = Make(event, timestamp);
    packet.event_ = event;
    return packet;
  }

  bool empty() const { return !data_; }
  Timestamp timestamp() const { return timestamp_; }
  ControlEvent event() const { return event_; }
  const MetadataSet& metadata() const { return metadata_; }
  MetadataSet& mutable_metadata() { return metadata_; }
  void AddLifetimeToken(std::shared_ptr<void> token) {
    if (token) lifetime_tokens_.push_back(std::move(token));
  }
  void InheritLifetimeTokens(const Packet& packet) {
    lifetime_tokens_.insert(lifetime_tokens_.end(), packet.lifetime_tokens_.begin(),
                            packet.lifetime_tokens_.end());
  }

  template <typename T>
  bool Is() const {
    return type_ == std::type_index(typeid(T));
  }

  template <typename T>
  const T& Get() const {
    if (!Is<T>()) {
      throw std::bad_cast();
    }
    return *std::static_pointer_cast<const T>(data_);
  }

 private:
  std::shared_ptr<const void> data_;
  std::type_index type_;
  Timestamp timestamp_ = Timestamp::Unset();
  MetadataSet metadata_;
  ControlEvent event_ = ControlEvent::kNone;
  std::vector<std::shared_ptr<void>> lifetime_tokens_;
};

}  // namespace rkavp
