# ADR-0001: 日志库与门面

- 状态：Accepted
- 日期：2026-08-10

## 背景

RK-AVP 需要线程安全日志、严重级别、源码位置、节点上下文和可替换 sink，同时不能让业务节点绑定某个第三方日志 API。

Google 相关项目存在两套常见选择：

- Abseil Logging：Google 当前 C++ 公共基础库的日志实现，提供 LOG、VLOG、条件/采样日志和 LogSink。当前 MediaPipe 新代码大量使用 `absl/log/*`。
- glog：历史悠久，MediaPipe 兼容层仍使用，但官方仓库已经归档，不作为新框架的首选依赖。

## 决策

公共 API 使用 `RKAVP_LOG` 和 `rkavp::LogSink`，不暴露 Abseil 或 glog 类型。

当前默认实现保持轻量，只依赖 C++17 标准库，提供级别、UTC 时间、线程 ID、源码位置和媒体上下文。后续生产环境需要 Abseil 的 VLOG、采样日志或生态 sink 时，在实现层接入 Abseil Logging，公共 API 保持不变。

## 结果

- 核心不会因为日志库升级产生公共 ABI 扩散。
- 嵌入式最小镜像不强制携带完整 Abseil。
- 可以通过自定义 sink 接入 journald 或集中日志。
- 需要维护一层很小的日志适配代码。

## 参考

- https://abseil.io/docs/cpp/guides/logging
- https://github.com/abseil/abseil-cpp
- https://github.com/google/glog
- https://developers.google.com/edge/mediapipe/framework/getting_started/troubleshooting
