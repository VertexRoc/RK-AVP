# Rockchip backend rules

## MPP

- Decode compressed packets into MPP or DMA-BUF backed `VideoFrame` values.
- Encode NV12/NV21 frames and drain delayed output after EOS.
- Keep codec context ownership inside the backend object.

## RGA

- Pass fd, stride, format, crop rectangle, and destination geometry explicitly.
- Use RGA for resize, colorspace conversion, crop, rotation, and overlays.
- Reuse destination buffers; CPU fallback must be visible in metrics.

## RKNN

- Load and initialize each model once.
- Cache tensor attributes and reuse input/output memory.
- Expose model-independent multi-input and multi-output `TensorSet` values with quantization and stride metadata.
- Preserve source frame ID and PTS as generic Packet metadata when an application needs correlation.
- Return failures as `Status`; never terminate a media worker.

Verify functional behavior through backend interfaces on x86. Mark real device tests with `hardware` and the SoC label.
