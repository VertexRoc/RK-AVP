#include <algorithm>
#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

#include "rkavp/core.hpp"

namespace {

std::atomic<bool> g_stop{false};
void SignalHandler(int) { g_stop = true; }

int PrintError(const rkavp::Status& status) {
  std::cerr << "error: " << status.message() << '\n';
  return 1;
}

void PrintUsage() {
  std::cout
      << "usage:\n"
      << "  rkavp-run doctor\n"
      << "  rkavp-run plugins [--plugin <library.so>]\n"
      << "  rkavp-run validate --graph <file.yaml> [--plugin <library.so>]\n"
      << "  rkavp-run inspect  --graph <file.yaml> [--plugin <library.so>]\n"
      << "  rkavp-run run      --graph <file.yaml> [--plugin <library.so>]\n"
      << "  rkavp-run trace    --graph <file.yaml> --output <trace.json> [--plugin <library.so>]\n";
}

struct Arguments {
  std::string graph;
  std::string output;
  std::vector<std::string> plugins;
};

bool ParseArguments(int argc, char** argv, Arguments* arguments) {
  for (int index = 2; index < argc; ++index) {
    const std::string option = argv[index];
    if ((option == "--graph" || option == "--output" || option == "--plugin") && index + 1 >= argc)
      return false;
    if (option == "--graph")
      arguments->graph = argv[++index];
    else if (option == "--output")
      arguments->output = argv[++index];
    else if (option == "--plugin")
      arguments->plugins.push_back(argv[++index]);
    else
      return false;
  }
  return true;
}

void AppendPluginPath(const std::string& path, std::vector<std::string>* plugins) {
  if (path.empty()) return;
  const std::filesystem::path candidate(path);
  if (std::filesystem::is_regular_file(candidate)) {
    plugins->push_back(candidate.string());
    return;
  }
  if (!std::filesystem::is_directory(candidate)) return;
  for (const auto& entry : std::filesystem::directory_iterator(candidate)) {
    if (!entry.is_regular_file() || entry.path().extension() != ".so") continue;
    const std::string name = entry.path().filename().string();
    if (name.rfind("librkavp_", 0) != 0 || name == "librkavp_core.so") continue;
    plugins->push_back(entry.path().string());
  }
}

void AppendDefaultPluginPaths(std::vector<std::string>* plugins) {
  std::error_code error;
  const std::filesystem::path executable = std::filesystem::canonical("/proc/self/exe", error);
  if (error) return;
  const std::filesystem::path binary_directory = executable.parent_path();
  AppendPluginPath(binary_directory.string(), plugins);
  AppendPluginPath((binary_directory.parent_path() / "lib").string(), plugins);
}

rkavp::Status LoadPlugins(const Arguments& arguments, rkavp::PluginManager* manager) {
  std::vector<std::string> plugins = arguments.plugins;
  AppendDefaultPluginPaths(&plugins);
  if (const char* paths = std::getenv("RKAVP_PLUGIN_PATH")) {
    std::string value(paths);
    std::size_t start = 0;
    while (start <= value.size()) {
      const std::size_t separator = value.find(':', start);
      AppendPluginPath(value.substr(start, separator - start), &plugins);
      if (separator == std::string::npos) break;
      start = separator + 1;
    }
  }
  std::sort(plugins.begin(), plugins.end());
  plugins.erase(std::unique(plugins.begin(), plugins.end()), plugins.end());
  for (const auto& plugin : plugins) {
    rkavp::Status status = manager->Load(plugin);
    if (!status.ok()) return status;
  }
  return rkavp::Status::Ok();
}

}  // namespace

int main(int argc, char** argv) {
  const char* log_level = std::getenv("RKAVP_LOG_LEVEL");
  rkavp::InitializeLogging({rkavp::ParseLogLevel(log_level == nullptr ? "info" : log_level), true});
  if (argc < 2) {
    PrintUsage();
    return 2;
  }
  const std::string command = argv[1];
  if (command == "doctor") {
    std::cout << "rkavp " << RKAVP_VERSION << "\ntarget_soc=" << RKAVP_TARGET_SOC << "\ncore=ok\n"
              << rkavp::FormatPlatformCapabilities(rkavp::ProbePlatformCapabilities());
    return 0;
  }

  Arguments arguments;
  if (!ParseArguments(argc, argv, &arguments)) {
    PrintUsage();
    return 2;
  }
  rkavp::NodeRegistry registry;
  rkavp::RegisterBuiltinNodes(&registry);
  rkavp::PluginManager plugins(&registry);
  rkavp::Status status = LoadPlugins(arguments, &plugins);
  if (!status.ok()) return PrintError(status);
  if (command == "plugins") {
    for (const auto& type : registry.Types()) std::cout << type << '\n';
    return 0;
  }
  if (arguments.graph.empty()) {
    PrintUsage();
    return 2;
  }

  rkavp::GraphConfig config;
  status = rkavp::YamlGraphLoader::LoadFile(arguments.graph, &config);
  if (!status.ok()) return PrintError(status);
  rkavp::Graph graph(std::move(config), &registry);
  status = graph.Validate();
  if (!status.ok()) return PrintError(status);
  if (command == "inspect") {
    std::cout << graph.Inspect();
    return 0;
  }
  if (command == "validate") {
    std::cout << "valid\n";
    return 0;
  }
  if (command != "run" && command != "trace") {
    PrintUsage();
    return 2;
  }
  if (command == "trace" && arguments.output.empty()) {
    PrintUsage();
    return 2;
  }

  rkavp::GraphRunner runner(std::move(graph), &registry);
  runner.SetErrorCallback(
      [](const rkavp::Status& error) { std::cerr << "graph error: " << error.message() << '\n'; });
  status = runner.Start();
  if (!status.ok()) return PrintError(status);
  std::signal(SIGINT, SignalHandler);
  std::signal(SIGTERM, SignalHandler);
  while (!g_stop && runner.last_error().ok())
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
  runner.Stop();
  if (command == "trace") {
    std::ofstream output(arguments.output, std::ios::binary);
    if (!output)
      return PrintError(
          rkavp::Status::Unavailable("cannot open trace output: " + arguments.output));
    output << runner.trace().ToChromeTraceJson();
  }
  return runner.last_error().ok() ? 0 : PrintError(runner.last_error());
}
