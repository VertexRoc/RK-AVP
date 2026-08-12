# RK-AVP文档

文档按学习、设计、运维、参考和架构决策分层。第一次阅读时不要从硬件backend源码开始。

## 学习路径

1. [框架入门](getting-started/framework.md)：Packet、Node、Edge、Executor和GraphRunner。
2. [Node与插件测试](getting-started/node-testing.md)：NodeTestRunner和安装后插件契约。
3. [图运行时](design/graph-runtime.md)：队列、Timestamp、输入同步和生命周期。
4. [总体架构](design/architecture.md)：模块分层、依赖方向和扩展边界。
5. [硬件加速](design/hardware-acceleration.md)：V4L2、MPP、RGA、RKNN和DMA-BUF。
6. [部署教程](operations/deployment-guide.md)：构建Runtime、设备授权和运行业务图。

## 目录说明

```text
docs/
├── getting-started/  第一次构建和使用框架
├── design/           架构、运行时和硬件设计
├── maintenance/      贡献流程、开发环境、评审和排查工具
├── operations/       部署、CI、测试和量产
├── reference/        YAML、CMake和CLI速查
└── adr/              不可随意重写的架构决策记录
```

## 设计文档

- [总体架构](design/architecture.md)
- [图运行时](design/graph-runtime.md)
- [硬件加速](design/hardware-acceleration.md)
- [日志与可观测性](design/logging.md)
- [RK-AVP与MediaPipe对比](design/mediapipe-comparison.md)

## 运维与交付

- [部署教程](operations/deployment-guide.md)
- [CI教程](operations/ci-guide.md)
- [量产教程](operations/production-guide.md)
- [测试与发布门禁](operations/testing-and-deployment.md)

## 维护与贡献

- [贡献指南](../CONTRIBUTING.md)
- [维护者手册](maintenance/README.md)
- [开发环境](maintenance/development-environment.md)
- [代码与评审规范](maintenance/coding-and-review.md)
- [代码排查工具](maintenance/debugging-toolkit.md)

## 参考手册

- [YAML配置](reference/yaml-schema.md)
- [CMake选项](reference/cmake-options.md)
- [命令行工具](reference/cli.md)

## 架构决策

[ADR索引](adr/README.md)记录已经影响公共架构、依赖方向和交付方式的决策。ADR解释“为什么这样决定”，普通教程解释“怎么使用”。

## 按角色阅读

| 角色 | 建议文档 |
|---|---|
| 框架开发者 | 入门、图运行时、总体架构、MediaPipe对比、ADR |
| 框架维护者 | 贡献指南、维护者手册、图运行时、ADR、CI教程 |
| 节点/插件开发者 | 入门、Node测试、总体架构、硬件加速、YAML参考 |
| 业务开发者 | 入门、部署教程、CLI参考 |
| CI维护者 | CI教程、测试与部署设计、CMake参考 |
| 量产负责人 | 部署教程、量产教程、硬件加速、容器ADR |

## 仓库边界

```text
include/rkavp + src/core  平台无关运行时
src/modules               Rockchip、Audio、Streaming和OpenCL插件
include/rkavp/testing     可安装的Node测试API
src/testing               NodeTestRunner实现
tools                     框架命令行工具
graphs                    可复用示例图
examples                  最小API示例
docker + deploy           框架交付配置
```

具体模型、后处理、ASR文本、检测框和Web服务属于业务仓库，不进入RK-AVP核心。

安装内容按`Runtime`、`Development`、`Testing`、`Documentation`和`Examples`组件拆分。量产Runtime只安装运行组件；开发SDK组合安装前三项。

## 参考工程

| 工程 | 吸收内容 | 不复制内容 |
|---|---|---|
| MediaPipe | Graph、Node、Packet、Timestamp、FlowLimiter、Service、Profiler思想 | Bazel、Protobuf、Tasks和完整Calculator生态 |
| rknn_model_zoo | RKNN属性、Tensor内存复用、RGA预处理和SoC实践 | 具体模型和后处理Demo |
| reComputer-RK-CV | ARM64镜像、GHCR和Rockchip板端交付经验 | 固定相机、默认privileged和YOLO Web业务 |

## 文档维护规则

- 修改核心类型或依赖方向时更新`design/architecture.md`。
- 修改队列、调度或生命周期时更新`design/graph-runtime.md`。
- 修改硬件内存路径时更新`design/hardware-acceleration.md`。
- 修改Docker入口时更新`operations/deployment-guide.md`。
- 修改GitHub Actions时更新`operations/ci-guide.md`。
- 修改稳定配置或命令时同步更新`reference/`。
- 不兼容架构决策应新增ADR，不覆盖旧决策记录。
