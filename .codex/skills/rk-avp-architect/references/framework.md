# Framework model

Use MediaPipe concepts selectively:

- `GraphConfig` is the declarative graph.
- `NodeContract` is the calculator contract and owns port/caps validation.
- `Packet` is immutable shared payload plus timestamp and metadata.
- `GraphRunner` schedules nodes on named Executors; independent edge queues provide FlowLimiter-like backpressure.
- Input policies cover any-input, timestamp synchronization, and main-input-with-latest-side-input behavior.
- `NodeRegistry` keeps graph construction separate from implementations.

Do not reproduce MediaPipe's full framework surface. RK-AVP targets embedded Linux and keeps one clear execution model, explicit queue capacity, and explicit memory ownership.

Reference projects used for design decisions:

- `../mediapipe`: graph, calculator, packet, timestamp, and flow-limiter patterns.
- `../reComputer-RK-CV`: V4L2, MPP, RGA, RKNN, RTSP, and container deployment patterns.
- `../rknn_model_zoo`: RKNN runtime, RGA preprocessing, postprocessing, and CMake examples.
