#pragma once

#include <memory>
#include <string>
#include <vector>

#include "rkavp/node_registry.hpp"
#include "rkavp/status.hpp"

namespace rkavp {

// This versions the C++ plugin API entry point. It is not a compiler-independent ABI.
constexpr int kPluginApiVersion = 2;
constexpr int kPluginAbiVersion = kPluginApiVersion;
using PluginInitV2 = int (*)(NodeRegistry* registry);

class PluginManager {
 public:
  explicit PluginManager(NodeRegistry* registry);
  ~PluginManager();
  Status Load(const std::string& path);
  Status Unload(const std::string& path);
  void UnloadAll();
  std::vector<std::string> LoadedPaths() const;

 private:
  struct Plugin;
  NodeRegistry* registry_;
  std::vector<std::unique_ptr<Plugin>> plugins_;
};

}  // namespace rkavp

extern "C" {
// The symbol has C linkage, but the NodeRegistry parameter requires plugins to
// use a compatible C++ toolchain and the same RK-AVP SDK version.
typedef int (*rkavp_plugin_init_v2_fn)(rkavp::NodeRegistry* registry);
}
