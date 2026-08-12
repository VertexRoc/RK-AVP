---
name: rk-avp-architect
description: Build and review RK-AVP C++17 multimedia graphs, YAML pipelines, Rockchip MPP/RGA/RKNN integrations, ALSA/RTSP modules, bounded scheduling, DMA-BUF ownership, tests, Docker, and CI. Use whenever changing RK-AVP architecture, nodes, graph configs, hardware backends, or deployment files.
---

# RK-AVP Architect

Work from the repository root and read the affected contracts before editing.

## Required architecture

- Keep `rkavp_core` independent of Rockchip, ALSA, RTSP, Python, and business logic.
- Express media flow as YAML graphs of typed `Node` ports carrying ref-counted `Packet` values.
- Preserve `Timestamp`, `TimeBase`, source ID, frame ID, and transform metadata across every node.
- Use bounded queues on every asynchronous edge. State the backpressure policy explicitly for real-time branches.
- Keep video payloads in DMA-BUF/MppBuffer/RKNN memory where possible. Never hide fd ownership or duplicate/close behavior.
- Place hardware calls behind `IMpp*`, `IRgaBackend`, `IRknnBackend`, `IAlsaBackend`, or streaming backend interfaces so host tests can substitute fakes.
- Put reusable targets behind `RKAVP::*`; business code must not link third-party targets directly.
- Use C++17 for the data plane. Python may configure graphs and consume events, but must not run the per-pixel hot path.

## Development workflow

1. Read `references/framework.md` and the reference matching the requested module.
2. Read the relevant file under `docs/` and update it when an architectural contract changes.
3. Inspect existing node contracts and YAML before introducing a new node or port.
4. Update or add a unit test before changing shared core behavior.
5. Add an integration graph test for queueing, branching, or lifecycle changes.
6. Run `scripts/check.sh` before finishing.
7. For hardware-only changes, also provide a fake-backend test and identify the board test label.

## Performance rules

- Prefer zero-copy fan-out of immutable `Packet` and `Buffer` objects.
- Use RGA for resize, crop, colorspace conversion, rotation, and overlay.
- Use MPP for compressed video decode and encode.
- Use RKNN for NPU inference and reuse model context, tensor descriptions, and input/output memory.
- Isolate slow inference from capture and main encoding with a small `drop_oldest` queue.
- Do not add an abstraction unless it removes ownership ambiguity, repeated integration code, or scheduling complexity.

## Completion criteria

- CMake configure and build succeed on the relevant preset.
- CTest labels affected by the change pass.
- YAML errors include node or edge context.
- Stop closes edge queues, wakes executors and source nodes, drains encoders where required, and joins threads.
- New dependencies are pinned in `3rdparty/versions.cmake` and fetched below `3rdparty/.cache`.
