#include "rkavp/platform.hpp"

#include <dirent.h>
#include <dlfcn.h>
#include <sys/utsname.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <sstream>

namespace rkavp {
namespace {

bool Exists(const std::string& path) {
  std::error_code error;
  return std::filesystem::exists(path, error);
}

std::string ReadText(const std::string& path, bool replace_nul = false) {
  std::ifstream stream(path, std::ios::binary);
  if (!stream) return {};
  std::string value((std::istreambuf_iterator<char>(stream)), std::istreambuf_iterator<char>());
  if (replace_nul) {
    for (char& ch : value)
      if (ch == '\0') ch = ',';
  }
  while (!value.empty() && (value.back() == ',' || value.back() == '\n' || value.back() == '\r')) {
    value.pop_back();
  }
  return value;
}

std::string ReadFirstAvailable(const std::initializer_list<const char*>& paths) {
  for (const char* path : paths) {
    std::string value = ReadText(path);
    if (!value.empty()) return value;
  }
  return {};
}

std::string QueryMppVersion() {
  void* library = ::dlopen("librockchip_mpp.so", RTLD_LAZY | RTLD_LOCAL);
  if (library == nullptr) return {};
  using VersionFunction = const char* (*)();
  auto version = reinterpret_cast<VersionFunction>(::dlsym(library, "get_mpp_version"));
  const char* value = version == nullptr ? nullptr : version();
  const std::string result = value == nullptr ? std::string{} : value;
  ::dlclose(library);
  return result;
}

std::string QueryRgaVersion() {
  void* library = ::dlopen("librga.so", RTLD_LAZY | RTLD_LOCAL);
  if (library == nullptr) return {};
  using QueryFunction = const char* (*)(int);
  auto query = reinterpret_cast<QueryFunction>(::dlsym(library, "querystring"));
  const char* value = query == nullptr ? nullptr : query(1);
  const std::string result = value == nullptr ? std::string{} : value;
  ::dlclose(library);
  return result;
}

bool CanLoadLibrary(const char* name) {
  void* library = ::dlopen(name, RTLD_LAZY | RTLD_LOCAL);
  if (library == nullptr) return false;
  ::dlclose(library);
  return true;
}

}  // namespace

PlatformCapabilities ProbePlatformCapabilities() {
  PlatformCapabilities result;
  result.compatible = ReadText("/proc/device-tree/compatible", true);
  if (result.compatible.find("rk3588") != std::string::npos)
    result.soc = "rk3588";
  else if (result.compatible.find("rk3576") != std::string::npos)
    result.soc = "rk3576";
  else
    result.soc = "unknown";
  utsname system{};
  if (::uname(&system) == 0) result.kernel = system.release;
  DIR* directory = ::opendir("/dev");
  if (directory != nullptr) {
    while (dirent* entry = ::readdir(directory)) {
      const std::string name = entry->d_name;
      if (name.rfind("video", 0) == 0 || name.rfind("media", 0) == 0 ||
          name.rfind("v4l-subdev", 0) == 0 || name.rfind("camera", 0) == 0) {
        result.devices.push_back("/dev/" + name);
        if (name.rfind("video", 0) == 0) ++result.video_device_count;
      }
    }
    ::closedir(directory);
  }
  for (const std::string root : {"/dev/dri", "/dev/dma_heap", "/dev/snd", "/dev/v4l/by-id"}) {
    std::error_code error;
    for (const auto& entry : std::filesystem::directory_iterator(root, error)) {
      if (!error) result.devices.push_back(entry.path().string());
    }
  }
  std::sort(result.devices.begin(), result.devices.end());
  result.devices.erase(std::unique(result.devices.begin(), result.devices.end()),
                       result.devices.end());
  result.has_video = result.video_device_count != 0;
  result.has_drm = Exists("/dev/dri/renderD128") || Exists("/dev/dri/renderD129");
  result.has_rga = Exists("/dev/rga") || result.has_drm;
  result.has_npu = Exists("/dev/rknpu") || result.has_drm;
  result.has_audio = Exists("/dev/snd/controlC0");
  result.mpp_version = QueryMppVersion();
  result.rga_version = QueryRgaVersion();
  result.rknn_runtime = CanLoadLibrary("librknnrt.so") ? "loadable" : "unavailable";
  result.rknn_driver_version = ReadFirstAvailable({
      "/sys/kernel/debug/rknpu/driver_version",
      "/sys/kernel/debug/rknpu/version",
      "/proc/rknpu/version",
  });
  return result;
}

std::string FormatPlatformCapabilities(const PlatformCapabilities& value) {
  std::ostringstream output;
  output << "soc=" << value.soc << "\ncompatible=" << value.compatible
         << "\nkernel=" << value.kernel << "\nvideo=" << (value.has_video ? "yes" : "no")
         << "\ndrm=" << (value.has_drm ? "yes" : "no") << "\nrga=" << (value.has_rga ? "yes" : "no")
         << "\nnpu=" << (value.has_npu ? "yes" : "no")
         << "\naudio=" << (value.has_audio ? "yes" : "no")
         << "\nvideo_device_count=" << value.video_device_count
         << "\nmpp_version=" << (value.mpp_version.empty() ? "unavailable" : value.mpp_version)
         << "\nrga_version=" << (value.rga_version.empty() ? "unavailable" : value.rga_version)
         << "\nrknn_runtime=" << value.rknn_runtime << "\nrknn_driver_version="
         << (value.rknn_driver_version.empty() ? "unavailable" : value.rknn_driver_version)
         << "\ndevices=";
  for (std::size_t i = 0; i < value.devices.size(); ++i) {
    if (i != 0) output << ',';
    output << value.devices[i];
  }
  output << "\n";
  return output.str();
}

}  // namespace rkavp
