#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <string>
#include <vector>

#include "rkavp/core.hpp"

namespace {

struct Device {
  std::string host_path;
  std::string container_path;
  std::string logical_name;
  std::string device_class;
};

bool Matches(const std::string& name, const std::vector<std::string>& prefixes) {
  return std::any_of(prefixes.begin(), prefixes.end(),
                     [&](const std::string& prefix) { return name.rfind(prefix, 0) == 0; });
}

std::string DeviceClass(const std::string& path) {
  const std::string name = std::filesystem::path(path).filename().string();
  if (Matches(name, {"video", "media", "v4l-subdev", "camera"}) ||
      path.find("/dev/v4l/by-id/") == 0)
    return "camera";
  if (name == "rga") return "rga";
  if (name == "rknpu") return "rknpu";
  if (name == "mpp_service" || name == "iep" || name == "ion" ||
      path.find("/dma_heap/") != std::string::npos)
    return "mpp";
  if (path.find("/snd/") != std::string::npos) return "audio";
  if (path.find("/dri/") != std::string::npos) return "drm";
  return "other";
}

std::vector<Device> DiscoverDevices() {
  std::vector<Device> devices;
  std::error_code error;
  for (const auto& entry : std::filesystem::directory_iterator("/dev", error)) {
    const std::string name = entry.path().filename().string();
    if (Matches(name, {"video", "media", "v4l-subdev", "camera"}) || name == "rga" ||
        name == "rknpu" || name == "mpp_service" || name == "iep" || name == "ion") {
      const std::string path = entry.path().string();
      devices.push_back({path, path, name, DeviceClass(path)});
    }
  }
  for (const std::string directory : {"/dev/dri", "/dev/dma_heap", "/dev/snd"}) {
    for (const auto& entry : std::filesystem::directory_iterator(directory, error)) {
      if (!error) {
        const std::string path = entry.path().string();
        devices.push_back({path, path, entry.path().filename().string(), DeviceClass(path)});
      }
      error.clear();
    }
  }
  const std::filesystem::path by_id("/dev/v4l/by-id");
  for (const auto& entry : std::filesystem::directory_iterator(by_id, error)) {
    const std::string container = entry.path().string();
    const std::string host = std::filesystem::weakly_canonical(entry.path(), error).string();
    if (!error && !host.empty()) {
      devices.push_back({host, container, entry.path().filename().string(), "camera"});
    }
    error.clear();
  }
  std::sort(devices.begin(), devices.end(), [](const Device& left, const Device& right) {
    return left.container_path < right.container_path;
  });
  devices.erase(std::unique(devices.begin(), devices.end(),
                            [](const Device& left, const Device& right) {
                              return left.container_path == right.container_path;
                            }),
                devices.end());
  return devices;
}

std::vector<std::string> DiscoverVendorLibraries() {
  std::vector<std::string> libraries;
  std::error_code error;
  for (const std::string root : {"/usr/lib/aarch64-linux-gnu", "/usr/lib64", "/vendor/lib64"}) {
    for (const auto& entry : std::filesystem::directory_iterator(root, error)) {
      const std::string name = entry.path().filename().string();
      if (name.rfind("librga.so", 0) == 0 || name.rfind("librknnrt.so", 0) == 0 ||
          name.rfind("librockchip_mpp.so", 0) == 0)
        libraries.push_back(entry.path().string());
    }
    error.clear();
  }
  std::sort(libraries.begin(), libraries.end());
  libraries.erase(std::unique(libraries.begin(), libraries.end()), libraries.end());
  return libraries;
}

std::string Sanitize(std::string value) {
  for (char& ch : value) {
    if (!(ch >= 'a' && ch <= 'z') && !(ch >= 'A' && ch <= 'Z') && !(ch >= '0' && ch <= '9') &&
        ch != '_' && ch != '-' && ch != '.')
      ch = '-';
  }
  return value;
}

bool WriteCdi(const std::filesystem::path& output, const std::string& kind,
              const std::vector<Device>& devices, const std::vector<std::string>& libraries) {
  if (devices.empty()) return true;
  std::ofstream file(output / ("rkavp-" + kind + ".yaml"), std::ios::binary);
  if (!file) return false;
  file << "cdiVersion: \"0.6.0\"\nkind: \"rockchip.com/" << kind << "\"\ndevices:\n";
  const auto emit_edits = [&](const std::vector<Device>& selected) {
    file << "    containerEdits:\n      env:\n"
         << "        - RKAVP_DEVICE_CLASS=" << kind << "\n"
         << "        - LD_LIBRARY_PATH=/opt/rkavp/host-libs:/opt/rkavp/lib\n"
         << "      deviceNodes:\n";
    for (const auto& device : selected) {
      file << "        - path: " << device.container_path << "\n"
           << "          hostPath: " << device.host_path << "\n"
           << "          permissions: rw\n";
    }
    if (!libraries.empty()) file << "      mounts:\n";
    for (const auto& library : libraries) {
      file << "        - hostPath: " << library << "\n"
           << "          containerPath: /opt/rkavp/host-libs/"
           << std::filesystem::path(library).filename().string() << "\n"
           << "          options: [ro, bind]\n";
    }
  };
  file << "  - name: all\n";
  emit_edits(devices);
  if (kind == "camera") {
    for (const auto& device : devices) {
      if (device.container_path.find("/dev/v4l/by-id/") != 0) continue;
      file << "  - name: " << Sanitize(device.logical_name) << "\n";
      emit_edits({device});
    }
  }
  return true;
}

void Usage() {
  std::cout << "usage:\n"
            << "  rkavp-ctk discover\n"
            << "  rkavp-ctk doctor\n"
            << "  rkavp-ctk compatibility\n"
            << "  rkavp-ctk cdi generate --output <directory>\n";
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 2) {
    Usage();
    return 2;
  }
  const std::string command = argv[1];
  if (command == "doctor") {
    std::cout << rkavp::FormatPlatformCapabilities(rkavp::ProbePlatformCapabilities());
    return 0;
  }
  if (command == "compatibility") {
    const auto capabilities = rkavp::ProbePlatformCapabilities();
    std::cout << "rkavp=" << RKAVP_VERSION << "\nsoc=" << capabilities.soc << "\nmpp="
              << (capabilities.mpp_version.empty() ? "unavailable" : capabilities.mpp_version)
              << "\nrga="
              << (capabilities.rga_version.empty() ? "unavailable" : capabilities.rga_version)
              << "\nrknn_runtime=" << capabilities.rknn_runtime << "\nrknn_driver="
              << (capabilities.rknn_driver_version.empty() ? "unavailable"
                                                           : capabilities.rknn_driver_version)
              << '\n';
    return 0;
  }
  const auto devices = DiscoverDevices();
  if (command == "discover") {
    for (const auto& device : devices) {
      std::cout << device.device_class << ' ' << device.logical_name << ' ' << device.host_path
                << " -> " << device.container_path << '\n';
    }
    return 0;
  }
  if (command == "cdi" && argc == 5 && std::string(argv[2]) == "generate" &&
      std::string(argv[3]) == "--output") {
    const std::filesystem::path output = argv[4];
    std::error_code error;
    std::filesystem::create_directories(output, error);
    if (error) {
      std::cerr << "cannot create output directory: " << error.message() << '\n';
      return 1;
    }
    std::map<std::string, std::vector<Device>> groups;
    for (const auto& device : devices) groups[device.device_class].push_back(device);
    const auto libraries = DiscoverVendorLibraries();
    for (const std::string kind : {"camera", "rga", "mpp", "rknpu", "audio", "drm"}) {
      if (!WriteCdi(output, kind, groups[kind], libraries)) {
        std::cerr << "cannot write CDI specification for " << kind << '\n';
        return 1;
      }
    }
    std::cout << "generated CDI specifications in " << output << '\n';
    return 0;
  }
  Usage();
  return 2;
}
