#pragma once

#include <string>

#include "rkavp/media_types.hpp"
#include "rkavp/packet.hpp"
#include "rkavp/status.hpp"

namespace rkavp {

enum class V4l2IoMode { kMmap, kExportDmaBuf };

struct V4l2CaptureConfig {
  std::string device;
  int width = 0;
  int height = 0;
  int fps = 0;
  std::string format;
  V4l2IoMode io_mode = V4l2IoMode::kExportDmaBuf;
  std::size_t buffer_count = 4;
};

class IV4l2Backend {
 public:
  virtual ~IV4l2Backend() = default;
  virtual Status Open(const V4l2CaptureConfig& config) = 0;
  virtual Status Read(Packet* packet) = 0;
  virtual void RequestStop() = 0;
  virtual void Close() = 0;
};

}  // namespace rkavp
