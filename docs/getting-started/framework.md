# 框架入门

本文面向第一次阅读 RK-AVP 的开发者。先建立运行时心智模型，再运行最小图，最后说明业务应该放在哪里。

## 一句话理解

RK-AVP 把视频、音频和 Tensor 处理表示成一张有向图：数据装进 `Packet`，沿 `Edge` 进入 `Node`，`Executor` 负责调度节点，`GraphRunner` 负责整张图的生命周期。

```text
Graph input
    |
    v
  Packet -> Node -> Packet -> Node -> Graph output
             ^                  ^
             |                  |
          Executor           Executor
```

它和普通流水线的区别是：每条边都有独立有界队列，节点可以运行在不同线程池，多输入节点可以按时间戳同步，慢推理分支不必阻塞主码流。

## 先认识七个对象

### Packet

`Packet` 是图中的消息。它包含：

- 一个不可变共享载荷，例如 `VideoFrame`、`AudioFrame`、`TensorSet` 或业务自定义类型。
- 一个 `Timestamp`。
- 可选元数据，例如 source ID、frame ID 和媒体变换。
- EOS、Timestamp Bound 等控制事件。

Packet 扇出时共享载荷，不复制图像或音频内存。数据面代码不要把裸指针或没有所有权的 fd 放进 Packet。

### Node

`Node` 是最小处理单元，近似 MediaPipe 的 Calculator。节点通过 `NodeContract` 声明输入、输出、媒体类型和必需配置，通过 `NodeContext`读取当前输入并发送输出。

生命周期固定为：

```text
Configure -> Open -> Start -> Process -> Stop -> Close
```

- `Configure`：解析选项，不打开设备。
- `Open`：加载模型、打开设备、创建硬件 Context。
- `Start`：允许 Source Node 开始产出。
- `Process`：处理一组已准备好的输入。
- `Stop`：响应取消，停止产生新数据。
- `Close`：释放 fd、Buffer Pool、模型和网络连接。

### Port与MediaCaps

Port 是节点的命名输入输出。`MediaCaps`描述端口接受的视频、音频、Tensor或编码流类型，以及格式和内存类型约束。

Graph在启动前检查端口是否存在、方向是否正确、上下游Caps是否兼容。不要依赖运行到一半才发现格式错误。

### Edge

Edge连接两个Port。每条Edge拥有独立队列：

```yaml
queue:
  capacity: 4
  policy: drop_oldest
```

策略含义：

- `block`：队列满时等待，适合不能丢失的数据。
- `drop_oldest`：保留最新数据，适合实时预览和推理。
- `drop_newest`：保护队列中已经排队的数据。

媒体图不应使用无界队列。队列容量和丢帧策略是业务延迟模型的一部分。

### Executor

Executor是命名的有界线程池。节点在YAML中选择Executor：

```yaml
executors:
  - name: media
    threads: 2
    queue_capacity: 64
  - name: inference
    threads: 1
    queue_capacity: 8
```

相机、网络、推理和控制任务应根据阻塞特性隔离。线程越多不一定越快，硬件Context和节点是否可重入必须先确认。

### Graph

Graph描述节点、边、图输入、图输出、Side Packet、Service和Subgraph。`Graph::Validate()`只检查静态结构，不启动线程或打开设备。

### GraphRunner

`GraphRunner`把验证后的Graph实例化并运行，主要接口包括：

```text
Start
AddPacket / CloseInputStream / SetInputTimestampBound
ObserveOutput / WaitForObservedOutput
WaitUntilIdle / WaitUntilDone
AddSource / RemoveSource / RestartSource
GetRuntimeInfo / Metrics / Trace
Cancel / Stop
```

Observer使用独立有界队列，回调不会直接占用媒体Executor线程。

## 运行第一个图

安装主机依赖：

```bash
sudo apt-get update
sudo apt-get install -y cmake ninja-build libyaml-cpp-dev libgtest-dev \
  pybind11-dev python3-dev python3-pytest
```

构建并测试：

```bash
cmake --preset host-debug
cmake --build --preset host-debug --parallel
ctest --preset host-debug
```

运行最小示例：

```bash
build/host-debug-make/examples/rkavp-hello-graph graphs/hello_graph.yaml
```

对应YAML只有一个Passthrough节点：

```yaml
version: 1
graph:
  name: hello-graph
  inputs:
    input: echo.in
  outputs:
    output: echo.out
  nodes:
    - id: echo
      type: Passthrough
```

C++执行顺序是：

```text
注册节点 -> 加载YAML -> 验证Graph -> 注册Observer
        -> Start -> AddPacket -> CloseInputStream
        -> WaitUntilDone -> Stop
```

完整代码位于 `examples/hello_graph/main.cpp`。

## YAML version 2

新图建议使用version 2，以声明统一流控默认值：

```yaml
version: 2
graph:
  name: camera-pipeline
  flow_control:
    input_queue: {capacity: 4, policy: drop_oldest}
    edge_queue: {capacity: 4, policy: drop_oldest}
    observer_queue: {capacity: 8, policy: drop_newest}
    observe_timestamp_bounds: true

  executors:
    - {name: media, threads: 2, queue_capacity: 64}
    - {name: inference, threads: 1, queue_capacity: 8}

  inputs:
    input: source.in
  outputs:
    output: sink.out

  nodes:
    - {id: source, type: Passthrough, executor: media}
    - {id: sink, type: Passthrough, executor: inference}

  edges:
    - from: source.out
      to: sink.in
      queue: {capacity: 2, policy: drop_oldest}
```

单条Input或Edge的队列设置优先于`flow_control`默认值。

## 三种输入策略

- `any`：任一输入到达就调度，适合事件合并和无同步要求的节点。
- `sync`：按Timestamp等待全部必需输入，适合音视频或多传感器对齐。
- `latest`：主端口触发，其他端口使用最新值，适合视频帧配合低频控制参数。

同步节点必须正确传播Timestamp Bound和EOS，否则图可能一直等待不存在的输入。

## 硬件节点放在哪里

核心库只理解通用数据类型，硬件能力通过插件提供：

```text
rkavp_rockchip   V4L2 / MPP / RGA / RKNN
rkavp_audio      ALSA
rkavp_streaming  ZLMediaKit
rkavp_opencl     OpenCL
```

业务图可以组合这些节点，但核心库不能反向依赖它们。模型后处理、检测框、ASR文本和Web服务应放在仓库外业务插件或应用中。

## C++业务工程

安装RK-AVP后，外部工程只链接命名目标：

```cmake
find_package(RKAVP REQUIRED)
add_executable(my_pipeline main.cpp)
target_link_libraries(my_pipeline PRIVATE RKAVP::Core)
```

需要硬件节点时加载插件，不要直接包含仓库内部实现文件。仓库内的`tests/install-consumer`就是安装后使用方式的最小验证工程。

开发和测试仓库外Node时，额外链接测试组件：

```cmake
target_link_libraries(my_node_test PRIVATE RKAVP::Core RKAVP::Testing)
```

`NodeTestRunner`负责构造NodeContext、注入输入/Side Packet/Service/Resource Manager、捕获异步输出并执行生命周期。完整说明见[Node与插件测试](node-testing.md)。

## Python边界

Python适合：

- 加载和验证YAML。
- 启动、停止和等待Graph。
- 动态增删Source。
- 接收低频结构化结果、Metrics和Trace。

Python不应承担逐像素转换、逐帧DMA-BUF处理或高频音频采样。媒体热路径保留在C++节点中，避免GIL和不确定对象生命周期。

## 推荐阅读顺序

1. 本文和仓库中的`examples/hello_graph`。
2. [图运行时](../design/graph-runtime.md)，理解队列、Timestamp和生命周期。
3. [总体架构](../design/architecture.md)，理解模块边界。
4. [硬件加速](../design/hardware-acceleration.md)，理解DMA-BUF和Rockchip插件。
5. [部署教程](../operations/deployment-guide.md)，在板端运行。
6. [量产教程](../operations/production-guide.md)，准备量产。

## 常见误区

- RKNN只加速模型执行，不会自动加速图调度、预处理和后处理。
- 增加Executor线程数不能解决硬件Context不可重入问题。
- `doctor`看到设备不代表端到端硬件链路已经通过。
- Mock测试证明接口契约，不等于真实MPP、RGA或RKNN验证。
- 框架仓库不应加入某个YOLO、ASR或Web业务主流程。
