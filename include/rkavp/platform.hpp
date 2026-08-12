#pragma once

#include <cstddef>
#include <string>
#include <vector>

namespace rkavp {

struct PlatformCapabilities {
  std::string compatible;
  std::string soc;
  std::string kernel;
  bool has_video = false;
  bool has_drm = false;
  bool has_rga = false;
  bool has_npu = false;
  bool has_audio = false;
  std::size_t video_device_count = 0;
  std::string mpp_version;
  std::string rga_version;
  std::string rknn_runtime;
  std::string rknn_driver_version;
  std::vector<std::string> devices;
};

PlatformCapabilities ProbePlatformCapabilities();
std::string FormatPlatformCapabilities(const PlatformCapabilities& capabilities);

}  // namespace rkavp
