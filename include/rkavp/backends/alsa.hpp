#pragma once

#include <string>

#include "rkavp/media_types.hpp"
#include "rkavp/status.hpp"

namespace rkavp {

struct AlsaCaptureConfig {
  std::string device = "default";
  int sample_rate = 0;
  int channels = 0;
  std::string format;
  int frame_ms = 0;
};

class IAlsaBackend {
 public:
  virtual ~IAlsaBackend() = default;
  virtual Status Open(const AlsaCaptureConfig& config) = 0;
  virtual Status Read(AudioFrame* frame) = 0;
  virtual Status RecoverOverrun() = 0;
  virtual void RequestStop() = 0;
  virtual void Close() = 0;
};

}  // namespace rkavp
