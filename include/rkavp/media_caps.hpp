#pragma once

#include <algorithm>
#include <cstdint>
#include <string>
#include <vector>

#include "rkavp/buffer.hpp"

namespace rkavp {

enum class MediaKind { kAny, kEncodedVideo, kVideo, kAudio, kTensor, kMetadata };

struct MediaCaps {
  MediaKind kind = MediaKind::kAny;
  std::string format;
  int width = 0;
  int height = 0;
  int sample_rate = 0;
  int channels = 0;
  std::vector<MemoryType> memory_types;
  std::vector<std::int64_t> tensor_shape;

  static MediaCaps Any() { return {}; }

  bool CompatibleWith(const MediaCaps& other) const {
    if (kind != MediaKind::kAny && other.kind != MediaKind::kAny && kind != other.kind) {
      return false;
    }
    if (!format.empty() && !other.format.empty() && format != other.format) {
      return false;
    }
    if (width > 0 && other.width > 0 && width != other.width) {
      return false;
    }
    if (height > 0 && other.height > 0 && height != other.height) {
      return false;
    }
    if (sample_rate > 0 && other.sample_rate > 0 && sample_rate != other.sample_rate) {
      return false;
    }
    if (channels > 0 && other.channels > 0 && channels != other.channels) {
      return false;
    }
    if (!memory_types.empty() && !other.memory_types.empty()) {
      for (const auto type : memory_types) {
        if (std::find(other.memory_types.begin(), other.memory_types.end(), type) !=
            other.memory_types.end()) {
          return true;
        }
      }
      return false;
    }
    return true;
  }
};

}  // namespace rkavp
