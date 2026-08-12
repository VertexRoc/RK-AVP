# 硬件加速设计

RK-AVP 将硬件能力拆成可选插件，图拓扑由使用方决定。框架不预设某个模型或固定三路视频业务。

## 通用零拷贝路径

```text
V4L2 / encoded network input
  -> MPP decode or camera DMA-BUF
  -> immutable VideoFrame fan-out
       -> RGA transform/composite
       -> RKNN TensorSet inference
       -> MPP encode
       -> streaming output
```

## V4L2 与 MPP

V4L2 backend 支持 MMAP、DMA-BUF 导出、`poll`、QBUF/DQBUF 所有权和可取消停止。采集 Buffer 通过 lease RAII 在最后一个 Packet 引用释放后重新 QBUF。

MPP decoder 使用外部 Buffer Group，处理 info-change 并输出 MPP/DMA-BUF frame。Encoder 接受 MPP 或 DMA-BUF，配置码率、GOP、帧率和 codec，并在 EOS 后排空输出。

## RGA

RGA 接口只提供 Transform、Blit 和 Composite。调用显式携带 crop、stride、format、rotation 和 alpha；导入 handle 可缓存，异步调用通过 Fence 回传完成状态。RGA 不理解 Tensor 或业务元数据。

## RKNN

RKNN backend 查询普通和 native Tensor 属性，映射 Shape、layout、量化、stride 和 byte size。模型加载后复用 `rknn_tensor_mem` 并通过 `rknn_set_io_mem` 绑定输入输出，支持多输入、多输出、动态 Shape、core mask 和性能查询。

框架输出使用独立 Buffer，避免下一次 `rknn_run` 覆盖仍在图中消费的 Tensor。需要真正端到端 zero-copy 的外部插件可基于 DMA-BUF fd 扩展 `rknn_create_mem_from_fd`，但必须保持 Packet 生命周期和 cache sync 正确。

## ALSA、Streaming 与 OpenCL

ALSA 协商采样率、通道、格式和 period，使用设备时间戳并恢复 XRUN。ZLMediaKit 网络线程通过有界队列与图隔离，输入支持重连退避，输出支持关键帧请求回调。OpenCL 是可选通用计算插件，只提供 Kernel、Command Queue、Buffer 互操作和 Fence，不成为核心依赖。

`OpenClKernel` 节点接收和输出通用 `TensorSet`。YAML 只声明 Kernel 名称、源码、全局工作尺寸和输出 Buffer 大小；框架负责编译、提交和传播 Fence，不内置语义分割后处理、颜色混合公式或具体模型逻辑。业务可以通过外部插件封装更强的类型约束，同时保持核心调度器与 OpenCL Runtime 解耦。

## 所有权规则

- `DmaBuffer::Adopt` 接管 fd，`Duplicate` 创建独立 fd。
- fan-out 共享 Buffer，不复制媒体载荷。
- CPU 映射前后执行正确的 device/CPU cache sync。
- backend 不缓存没有引用保护的裸内存地址。
- Fence 在跨 RGA、NPU、GPU 或编码器边界前等待或继续传递。

`rkavp-run doctor` 报告 SoC compatible 字符串、视频/DRM/RGA/NPU/ALSA 设备可见性，并以运行时加载方式查询 MPP、RGA、RKNN Runtime 与常见 RKNPU 驱动版本节点。RKNN SDK 的精确版本查询要求已初始化模型 context，由 RKNN 板端测试记录。
