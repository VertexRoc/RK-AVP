#pragma once

#include <string>

#include "rkavp/media_types.hpp"
#include "rkavp/status.hpp"

namespace rkavp {

struct MppDecoderConfig {
  std::string codec;
  int width = 0;
  int height = 0;
  std::size_t buffer_count = 8;
  bool external_buffer_group = true;
  std::int64_t drain_timeout_ms = 2000;
};

struct MppEncoderConfig {
  std::string codec;
  int width = 0;
  int height = 0;
  int fps = 0;
  int bitrate = 0;
  int gop = 0;
  std::string profile;
  std::string rate_control = "cbr";
  std::int64_t drain_timeout_ms = 2000;
};

class IMppDecoderBackend {
 public:
  virtual ~IMppDecoderBackend() = default;
  virtual Status Open(const MppDecoderConfig& config) = 0;
  virtual Status Submit(const EncodedPacket& packet) = 0;
  virtual Status Receive(VideoFrame* frame, bool* end_of_stream) = 0;
  virtual Status Drain() = 0;
  virtual void Close() = 0;
};

class IMppEncoderBackend {
 public:
  virtual ~IMppEncoderBackend() = default;
  virtual Status Open(const MppEncoderConfig& config) = 0;
  virtual Status Submit(const VideoFrame& frame) = 0;
  virtual Status Receive(EncodedPacket* packet, bool* end_of_stream) = 0;
  virtual Status Drain() = 0;
  virtual Status ForceKeyFrame() = 0;
  virtual void Close() = 0;
};

}  // namespace rkavp
