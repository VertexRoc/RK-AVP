# RK-AVP

RK-AVP is a lightweight C++17 multimedia graph framework for Rockchip edge devices. It adopts MediaPipe-style graph composition and explicit contracts while keeping scheduling, queue capacity, hardware memory, and third-party dependencies visible to the application.

## Documentation

Start with the Chinese [framework guide](docs/getting-started/framework.md), then continue
with the [deployment tutorial](docs/operations/deployment-guide.md),
[CI tutorial](docs/operations/ci-guide.md),
[production guide](docs/operations/production-guide.md), and the detailed
[MediaPipe comparison](docs/design/mediapipe-comparison.md). The complete index is in
[docs/README.md](docs/README.md).

Contributors and maintainers should start with [CONTRIBUTING.md](CONTRIBUTING.md) and the
[maintenance handbook](docs/maintenance/README.md), which define the development environment,
review gates, ownership rules, and debugging tools.

```text
librkavp_core
  Packet / Timestamp / TimeBase / ConfigValue
  Buffer / DmaBuffer / BufferPool / MediaCaps / TensorSet
  Node / NodeContext / NodeContract / NodeRegistry
  Graph / named Executors / per-edge queues / Metrics / Trace

optional plugins
  rkavp_rockchip   V4L2, MPP, RGA, RKNN
  rkavp_audio      ALSA capture
  rkavp_streaming  RTSP/RTMP input and output through ZLMediaKit
  rkavp_opencl     generic OpenCL interop
  rkavp_python     Python control plane
```

The core has no dependency on MPP, RGA, RKNN, ALSA, ZLMediaKit, or OpenCL. External applications link only installed `RKAVP::*` CMake targets and load optional node plugins when needed. No model, media file, or model-specific postprocessing is part of the framework.

External Node tests can link `RKAVP::Testing`. Its `NodeTestRunner` constructs a
`NodeContext`, injects inputs and services, captures asynchronous outputs, and
drives the Node lifecycle without imposing a particular C++ test framework. See
the [Node and plugin testing guide](docs/getting-started/node-testing.md).

## Build

```bash
cmake --preset host-debug
cmake --build --preset host-debug --parallel
ctest --preset host-debug
```

Missing pinned dependencies can be downloaded below `3rdparty/src` with
`RKAVP_FETCH_DEPS=ON`. Dependency build trees remain inside the active CMake
build directory, so host and ARM64 builds do not share generated files. To
prepare source dependencies before an offline board build:

```bash
cmake --build build/host-debug-make --target rkavp-prefetch
```

Rockchip cross builds use the supplied presets:

```bash
cmake --preset rk3588-release
cmake --build --preset rk3588-release --parallel
```

Installed artifacts are componentized. A production prefix needs `Runtime`;
an SDK prefix combines `Runtime`, `Development`, and `Testing`:

```bash
cmake --install build/rk3588-release --prefix install/rk3588 --component Runtime
cmake --install build/rk3588-release --prefix install/rk3588 --component Development
cmake --install build/rk3588-release --prefix install/rk3588 --component Testing
```

## Graph Tools

```bash
rkavp-run doctor
rkavp-run validate --graph graphs/passthrough.yaml
rkavp-run inspect --graph graphs/passthrough.yaml
rkavp-run plugins --plugin /opt/rkavp/lib/librkavp_rockchip.so
rkavp-run run --graph graph.yaml --plugin /opt/rkavp/lib/librkavp_rockchip.so
rkavp-run trace --graph graph.yaml --output trace.json
```

YAML supports named executors, independent queues for every edge, `any`/`sync`/`latest` input policies, explicit back edges, graph inputs and outputs, side packets, services, and nested subgraphs.

YAML v2 additionally configures bounded graph-input queues and predeclared dynamic source slots. The runtime includes `FlowLimiter`, model-independent `PacketBatch`, adaptive mux/demux nodes, asynchronous output observers, runtime inspection, RTP timestamp unwrapping, and encoded jitter buffering.

## Python

Python controls graph construction and lifecycle. Per-frame media transforms stay in C++ nodes or external C++ plugins.

```bash
cmake -S . -B build/python -DRKAVP_ENABLE_PYTHON=ON
cmake --build build/python --parallel
PYTHONPATH=build/python python3 -c 'import rkavp; print(rkavp.validate("graphs/passthrough.yaml"))'
```

The binding also exposes graph inputs, input close, idle/done waits, cancellation,
metrics snapshots, and bounded asynchronous output callbacks for structured
control-plane packets. Callback exceptions are isolated from C++ executor threads.

## Examples

Framework examples are separate from tools and business applications:

```bash
cmake -S . -B build/examples -DRKAVP_BUILD_EXAMPLES=ON
cmake --build build/examples --parallel
build/examples/examples/rkavp-hello-graph graphs/hello_graph.yaml
```

`tools/` contains framework executables, `graphs/` contains reusable YAML
graphs, and `examples/` contains small API demonstrations. Product applications
should live in separate repositories and consume installed `RKAVP::*` targets.

## Containers

The root Dockerfile provides `ci`, `sdk-arm64`, `devel-rk3588/rk3576`, and
`runtime-rk3588/rk3576` targets. Runtime images contain the framework and
plugins, not business graphs or models. Build and run through one entry point:

```bash
docker/rkavp-docker build rk3588 rkavp:rk3588
RKAVP_IMAGE=rkavp:rk3588 docker/rkavp-docker run rk3588 doctor
```

The command automatically selects CDI or precise host-device discovery. See
[docker](docker/README.md) for debug mode and image publishing details.

Architecture and operating documentation starts at [docs/README.md](docs/README.md). Repository-local Codex development rules are in `.codex/skills/rk-avp-architect` and follow the official skill layout.
