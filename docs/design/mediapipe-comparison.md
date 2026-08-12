# RK-AVP与MediaPipe对比

RK-AVP不是MediaPipe的CMake移植，也不追求API兼容。它采用MediaPipe的图运行时思想，把平台层替换为Rockchip媒体和NPU能力，并缩小框架范围。

## 定位差异

| 维度 | MediaPipe | RK-AVP |
|---|---|---|
| 目标 | 跨桌面、移动端、Web的通用媒体与ML框架 | RK3588/RK3576多媒体与AI运行框架 |
| 构建 | Bazel | CMake、C++17 |
| 图配置 | Protobuf `CalculatorGraphConfig` | YAML `GraphConfig` |
| 执行单元 | Calculator/Node API | Node/NodeContext |
| 数据 | Packet、Image、Tensor及大量Proto格式 | Packet、Buffer、VideoFrame、AudioFrame、TensorSet |
| 类型约束 | Tag、Index、模板和Proto类型 | NodeContract、MediaCaps和运行前验证 |
| 平台加速 | CPU、OpenGL、Metal、Android/iOS等 | V4L2、MPP、RGA、RKNN、DMA-BUF、OpenCL |
| 扩展 | Calculator注册、Subgraph、Tasks生态 | NodeRegistry、版本化C++插件API、Subgraph |
| Python | Solution/Tasks和框架绑定 | 只做控制面与低频结构化数据 |
| 部署 | SDK、移动端库、Python包和开发容器 | ARM64 Runtime、CDI、板端设备发现和GHCR |

## 核心概念映射

| MediaPipe | RK-AVP | 状态 |
|---|---|---|
| `Packet` | `Packet` | 已吸收不可变共享载荷和Timestamp |
| `Timestamp` | `Timestamp`/`TimeBase` | 已吸收特殊值、单调性和转换 |
| `CalculatorBase`/Node API | `Node` | 已吸收契约和生命周期 |
| `CalculatorContext` | `NodeContext` | 已吸收输入集、输出和服务访问 |
| `CalculatorGraph` | `Graph`/`GraphRunner` | 已吸收验证、启动、输入、输出和停止 |
| Input Stream Handler | `any/sync/latest` | 已实现常用策略，种类少于MediaPipe |
| Stream queue | 每Edge独立有界队列 | 已吸收，并显式暴露容量与丢帧策略 |
| Timestamp Bound | Timestamp Bound/EOS | 已吸收 |
| Side Packet | `SidePacketSet` | 已吸收不可变启动参数 |
| Graph Service | `GraphServiceRegistry` | 已吸收进程内共享服务 |
| Resources Service | `ResourceManager` | 已吸收资源解析边界 |
| Subgraph | Subgraph展开 | 已吸收端口映射和配置覆盖 |
| Flow Limiter | `FlowLimiter` | 已吸收慢分支在途限制 |
| Output Stream Poller/Observer | 异步Observer | 已吸收并使用独立有界队列 |
| Executor | 命名Executor线程池 | 已吸收并增加显式容量和饥饿指标 |
| Graph Profiler/Tracer | Metrics/Chrome Trace | 已吸收基础指标和环形Trace |
| Calculator Registry | NodeRegistry/版本化插件API | 已吸收，支持仓库外动态插件；当前不是跨编译器稳定C ABI |

## Graph写法差异

MediaPipe通常使用Proto：

```textproto
input_stream: "in"
output_stream: "out"
node {
  calculator: "PassThroughCalculator"
  input_stream: "in"
  output_stream: "out"
}
```

RK-AVP使用YAML：

```yaml
version: 1
graph:
  inputs: {input: pass.in}
  outputs: {output: pass.out}
  nodes:
    - {id: pass, type: Passthrough}
```

选择YAML的原因是Rockchip部署人员更容易直接编辑，CMake工程也不需要引入Protobuf代码生成。代价是编译期类型能力弱于MediaPipe，配置兼容需要由版本号和验证器维护。

## 调度差异

MediaPipe拥有多种成熟Input Stream Handler，例如default、immediate、barrier、fixed-size、sync-set和timestamp-align。RK-AVP当前只提供最常用的：

- `any`对应低同步约束的即时处理。
- `sync`对应按Timestamp组合必需输入。
- `latest`对应主输入触发、辅助输入取最近值。

RK-AVP额外强调每条Edge的容量、丢帧和统计，因为边缘设备的视频分支必须明确控制延迟和内存。需要新的同步语义时，应增加小而可测试的策略，不复制MediaPipe全部Handler。

## 类型系统差异

MediaPipe API2/API3允许节点用模板常量声明强类型端口和Tag，编译期体验更完整。RK-AVP的NodeContract和MediaCaps主要在Graph验证阶段检查，适合动态YAML和仓库外插件，但C++编译器不能捕获所有端口拼写错误。

这是目前最值得继续借鉴MediaPipe的方向之一：可以在不改变YAML的前提下，为C++节点增加可选的类型化Port声明和Builder API。不过这应建立在现有运行时稳定之后，避免引入第二套不兼容API。

## 硬件层差异

MediaPipe需要覆盖大量平台，因此GPU和图像抽象更通用。RK-AVP专门面向Rockchip：

```text
V4L2/网络输入
  -> MPP decode
  -> DMA-BUF Packet fan-out
  -> RGA transform/composite
  -> RKNN TensorSet inference
  -> MPP encode
  -> Streaming output
```

RK-AVP在这部分不是照搬MediaPipe，而是吸收`rknn_model_zoo`和`reComputer-RK-CV`的板端实践：Tensor属性查询、RGA预处理、MPP/RKNN运行库、SoC拆分和ARM64镜像交付。

## 部署差异

MediaPipe仓库中的Docker主要用于构建和开发环境，真正产品部署通常由Android、iOS、Python或业务应用负责。RK-AVP必须直接面对Linux板端设备节点，因此增加：

- 多相机设备发现。
- RGA、MPP、RKNN、DRM、DMA-HEAP和ALSA授权。
- CDI生成。
- RK3588/RK3576 Runtime镜像。
- 自托管硬件Runner。

这些不是偏离框架，而是Rockchip平台层职责。对普通使用者只暴露`docker/rkavp-docker build/run`，内部才处理Compose和CDI细节。

## 明确不复制的内容

- Bazel和Protobuf构建体系。
- MediaPipe Tasks、Solutions和完整Calculator目录。
- 人脸、手势、检测、分割等业务数据结构。
- Android、iOS、Web和所有GPU平台抽象。
- 任意运行时Graph热修改。
- Python逐帧媒体热路径。

这些内容会显著扩大依赖和维护面，也不符合RK-AVP当前的Rockchip框架定位。

## 仍值得借鉴的部分

按优先级建议：

1. 更完整的节点级测试Harness，简化单个Node输入输出测试。
2. 可选的类型化Port声明和C++ Graph Builder。
3. 图可视化和Trace联动，帮助分析复杂多流图。
4. 更丰富但保持少量的时间戳同步策略。
5. Graph模板参数化和更清晰的嵌套错误路径。
6. 更系统的Profiler报告和性能回归基线。

不建议在真实硬件、长稳和零拷贝验证完成前优先扩展大型Calculator生态。

## 性能结论

采用MediaPipe式架构不会自动带来“质的飞跃”。框架能改善的是：

- 避免不必要媒体载荷复制。
- 隔离慢分支。
- 控制队列和延迟。
- 提高多流资源利用率。
- 让MPP、RGA和RKNN可以组成可维护流水线。

模型单次NPU执行时间仍由模型、量化、RKNN Runtime、频率和core mask决定。RK-AVP是否比MediaPipe或手写流水线更快，必须通过同一输入、同一模型和同一硬件上的端到端基准证明。

## 最终定位

准确描述是：

> RK-AVP是一个MediaPipe式的轻量C++17图运行时，使用CMake和YAML，平台层针对Rockchip的MPP、RGA、RKNN和DMA-BUF重新设计。

它追求的是相同的组合思想和运行时纪律，不追求源码、API或生态规模一致。
