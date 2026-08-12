#pragma once

#include <string>

#include "rkavp/media_types.hpp"
#include "rkavp/status.hpp"

namespace rkavp {

struct Rect {
  int x = 0;
  int y = 0;
  int width = 0;
  int height = 0;
};

struct RgaTransformRequest {
  Rect source_rect;
  Rect destination_rect;
  int destination_width = 0;
  int destination_height = 0;
  std::string destination_format;
  int rotation_degrees = 0;
  bool asynchronous = false;
};

struct RgaCompositeRequest {
  Rect foreground_rect;
  Rect background_rect;
  Rect destination_rect;
  float alpha = 1.0F;
  bool asynchronous = false;
};

class IRgaBackend {
 public:
  virtual ~IRgaBackend() = default;
  virtual Status Transform(const VideoFrame& source, const RgaTransformRequest& request,
                           VideoFrame* destination) = 0;
  virtual Status Blit(const VideoFrame& source, const RgaTransformRequest& request,
                      VideoFrame* destination) = 0;
  virtual Status Composite(const VideoFrame& foreground, const VideoFrame& background,
                           const RgaCompositeRequest& request, VideoFrame* destination) = 0;
  virtual void ReleaseCachedHandles() = 0;
};

}  // namespace rkavp
