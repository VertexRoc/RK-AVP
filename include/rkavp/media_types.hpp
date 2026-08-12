#pragma once

#include <cstdint>
#include <string>

#include "rkavp/buffer.hpp"
#include "rkavp/timestamp.hpp"

namespace rkavp {

struct VideoFrame {
  std::uint64_t frame_id = 0;
  Timestamp pts;
  int width = 0;
  int height = 0;
  int horizontal_stride = 0;
  int vertical_stride = 0;
  std::string format;
  BufferPtr buffer;
};

struct AudioFrame {
  Timestamp pts;
  int sample_rate = 0;
  int channels = 0;
  int samples_per_channel = 0;
  std::string format;
  BufferPtr buffer;
};

struct EncodedPacket {
  Timestamp pts;
  Timestamp dts;
  std::string codec;
  bool key_frame = false;
  BufferPtr buffer;
};

}  // namespace rkavp
