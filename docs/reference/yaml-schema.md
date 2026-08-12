# YAML配置参考

## 顶层

```yaml
version: 2
graph:
  name: example
  flow_control: {}
  executors: []
  inputs: {}
  outputs: {}
  side_packets: {}
  services: {}
  nodes: []
  edges: []
  subgraphs: []
  source_slots: []
```

`version: 1`保留兼容；`flow_control`和`source_slots`要求version 2。

## Executor

```yaml
executors:
  - name: media
    threads: 2
    queue_capacity: 64
    priority: 0
```

节点通过`executor: media`选择线程池。没有声明时使用默认Executor。

## Node

```yaml
nodes:
  - id: resize
    type: RgaTransform
    executor: media
    options:
      width: 640
      height: 640
      rotation: 0
```

`options`支持bool、整数、浮点、字符串、数组和对象。字符串支持`${NAME}`和`${NAME:-default}`环境变量替换。

## Graph输入输出

```yaml
inputs:
  input: first.in
outputs:
  output: last.out
```

version 2输入可以声明独立队列；未声明时使用`flow_control.input_queue`。

## Edge

```yaml
edges:
  - from: source.out
    to: sink.in
    queue:
      capacity: 4
      policy: drop_oldest
```

队列策略为`block`、`drop_oldest`或`drop_newest`。普通环路会被拒绝；反馈边必须显式声明`back_edge`并提供初始Packet或Delay语义。

## Flow control

```yaml
flow_control:
  input_queue: {capacity: 4, policy: drop_oldest}
  edge_queue: {capacity: 4, policy: drop_oldest}
  observer_queue: {capacity: 8, policy: drop_newest}
  observe_timestamp_bounds: true
```

单条Input或Edge配置优先于全局默认值。

## 输入策略

节点Contract可选择：

- `any`：任一端口到达即调度。
- `sync`：按Timestamp组合必需输入。
- `latest`：主端口触发，其他端口取最近值。

输入策略由节点类型声明，不由任意YAML覆盖，以保护节点契约。

## Side packet与Service

```yaml
side_packets:
  threshold: 0.25
  labels: [person, vehicle]
```

Side Packet在图启动后不可变。Service由宿主进程注册，YAML只引用框架允许公开的服务配置。

## Source slot

```yaml
source_slots:
  - name: camera
    target_input: input
    subgraph: graphs/camera_source.yaml
    output: frame
```

动态增删只发生在预声明槽位，不支持任意Node和Edge热修改。

## 验证

```bash
rkavp-run validate --graph graph.yaml
rkavp-run inspect --graph graph.yaml
```

验证覆盖版本、节点类型、端口、Executor、配置、Caps、重复ID、环路和Subgraph映射。
