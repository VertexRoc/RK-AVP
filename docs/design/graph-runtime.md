# 图运行时

## 构建阶段

```text
YAML + environment
  -> structured GraphConfig
  -> recursive Subgraph expansion
  -> node/executor/port/MediaCaps validation
  -> back-edge and cycle validation
  -> runtime nodes, edges and observers
```

验证在工作线程启动前完成，错误应包含节点、端口、边或嵌套 Subgraph 路径。

## 调度模型

命名 Executor 是固定大小的有界线程池。节点在 YAML 中选择 Executor；同一节点默认串行执行，可重入节点可以让多个输入批次并行。Executor 可配置线程数、任务队列容量和 best-effort 优先级。

Edge 队列不合并到节点 inbox：每条边独立执行 `block`、`drop_oldest` 或 `drop_newest`，并记录队列等待、深度和丢包。慢 AI、Python 或网络分支因此不会自动反压不相关的主媒体分支。

## 输入策略

- `any` 将当前到达的端口组成一次输入集。
- `sync` 缓存各端口 Packet，并按 Timestamp 组合全部必需输入。
- `latest` 由 `trigger_port` 触发，同时附带其他端口的最新 Packet。

Timestamp bound、stream close 和 EOS 告诉同步器某个时间点之后不会再有输入，防止多输入节点永久等待。

## 生命周期

```text
Created -> Configured -> Open -> Running -> Stopped -> Closed
                                | error/cancel
                                v
                              Stopping
```

`Configure` 只解析配置，`Open` 获取设备和模型资源，`Start` 允许 Source Node 产出数据，`Stop` 发出取消并唤醒队列，`Close` 释放 fd、context、buffer group 和网络会话。启动失败必须逆序关闭已经打开的节点。

图控制接口包括 `AddPacket`、`SetInputTimestampBound`、`CloseInputStream`、`ObserveOutput`、`WaitForObservedOutput`、`WaitUntilIdle`、`WaitUntilDone`、`Cancel`、`GetRuntimeInfo` 和 `SetErrorCallback`。观察者有独立有界队列和可取消`ObserverHandle`，回调不会占用图Executor线程。

YAML `version: 2`允许图输入声明独立队列，并增加`source_slots`和`flow_control`。`flow_control`统一设置`input_queue`、`edge_queue`、`observer_queue`及`observe_timestamp_bounds`默认值，单条Input或Edge配置优先。Observer运行在独立工作线程中，其队列只允许`drop_oldest`或`drop_newest`，禁止`block`阻塞媒体Executor。加载器继续接受v1，使用兼容默认队列容量和策略。

## 多流与批处理

`PacketBatch`保留每项原Packet、`source_id`、PTS、媒体描述和批内索引。`AdaptiveBatch`支持最大Batch、首包超时、Partial Batch、每源上限和源间轮询；`RknnBatchInference`在后端不支持原生Batch时逐项执行，并保持Source/Frame映射。

动态Source通过预声明槽位增删、重启和查询健康状态。状态使用`connecting/streaming/stalled/reconnecting/eos/failed/stopped`，删除流程先停止输入，再排空并释放Source资源，不改变固定下游图。

## 反馈与子图

普通环路在验证阶段拒绝。反馈边必须声明 `back_edge`，并提供初始 Packet 或经过 Delay 节点。Subgraph 在构建阶段真正展开，映射公开端口并应用实例配置覆盖；展开后仍使用相同的 Edge 队列和 Executor 规则。
