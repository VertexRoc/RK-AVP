#include "rkavp/plugin.hpp"

#include <dlfcn.h>

#include <algorithm>
#include <set>

namespace rkavp {

struct PluginManager::Plugin {
  std::string path;
  void* handle = nullptr;
  std::vector<std::string> registered_types;
};

PluginManager::PluginManager(NodeRegistry* registry) : registry_(registry) {}
PluginManager::~PluginManager() { UnloadAll(); }

Status PluginManager::Load(const std::string& path) {
  if (registry_ == nullptr || path.empty())
    return Status::Invalid("plugin registry and path are required");
  for (const auto& plugin : plugins_)
    if (plugin->path == path) return Status::AlreadyExists("plugin is already loaded: " + path);
  // NodeRegistry returns ordinary unique_ptr<Node> objects, so it cannot know
  // when the final plugin-owned instance is destroyed. Keep the code mapped
  // after logical unload to make those instances safe to destroy or invoke.
  void* handle = ::dlopen(path.c_str(), RTLD_NOW | RTLD_LOCAL | RTLD_NODELETE);
  if (handle == nullptr) return Status::Unavailable(std::string("dlopen failed: ") + ::dlerror());
  ::dlerror();
  auto init = reinterpret_cast<PluginInitV2>(::dlsym(handle, "rkavp_plugin_init_v2"));
  const char* symbol_error = ::dlerror();
  if (symbol_error != nullptr || init == nullptr) {
    ::dlclose(handle);
    return Status::Invalid("incompatible plugin API; expected rkavp_plugin_init_v2: " + path);
  }
  const std::vector<std::string> before = registry_->Types();
  if (init(registry_) != 0) {
    const std::vector<std::string> after = registry_->Types();
    std::vector<std::string> partial_types;
    std::set_difference(after.begin(), after.end(), before.begin(), before.end(),
                        std::back_inserter(partial_types));
    for (const auto& type : partial_types) registry_->Unregister(type);
    ::dlclose(handle);
    return Status::Internal("plugin initialization failed: " + path);
  }
  const std::vector<std::string> after = registry_->Types();
  auto plugin = std::make_unique<Plugin>();
  plugin->path = path;
  plugin->handle = handle;
  std::set_difference(after.begin(), after.end(), before.begin(), before.end(),
                      std::back_inserter(plugin->registered_types));
  plugins_.push_back(std::move(plugin));
  return Status::Ok();
}

Status PluginManager::Unload(const std::string& path) {
  const auto it = std::find_if(plugins_.begin(), plugins_.end(),
                               [&](const auto& plugin) { return plugin->path == path; });
  if (it == plugins_.end()) return Status::NotFound("plugin is not loaded: " + path);
  for (const auto& type : (*it)->registered_types) registry_->Unregister(type);
  ::dlclose((*it)->handle);
  plugins_.erase(it);
  return Status::Ok();
}

void PluginManager::UnloadAll() {
  while (!plugins_.empty()) Unload(plugins_.back()->path);
}

std::vector<std::string> PluginManager::LoadedPaths() const {
  std::vector<std::string> paths;
  for (const auto& plugin : plugins_) paths.push_back(plugin->path);
  return paths;
}

}  // namespace rkavp
