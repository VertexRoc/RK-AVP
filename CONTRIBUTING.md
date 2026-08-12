# 参与RK-AVP开发

RK-AVP是框架仓库。提交应保持核心平台无关、硬件能力插件化、媒体数据面模型无关。具体模型、后处理、Web服务和产品逻辑应放在业务或Samples仓库。

完整维护手册见[维护者文档](docs/maintenance/README.md)。

## 标准流程

1. 阅读[总体架构](docs/design/architecture.md)、[图运行时](docs/design/graph-runtime.md)及相关ADR。
2. 从最新目标分支创建短生命周期分支，每个提交只处理一个可独立解释的问题。
3. 先添加能够复现问题或约束新行为的测试，再修改实现。
4. 执行格式、构建、测试和安装后消费测试。
5. 更新受影响的设计、运维或参考文档。
6. 提交Pull Request，说明行为变化、风险、验证结果和未验证项。

最小本地门禁：

```bash
cmake --preset host-debug
cmake --build --preset host-debug --parallel
ctest --preset host-debug

find include src tools examples tests -type f \
  \( -name '*.hpp' -o -name '*.cpp' \) -print0 \
  | xargs -0 clang-format --dry-run --Werror
```

涉及Python、并发、内存或Rockchip模块时，还必须执行对应专项检查，参见[代码与评审规范](docs/maintenance/coding-and-review.md)。

## 提交边界

可以进入主仓库：

- 通用Packet、Graph、Executor、Buffer、Service和测试能力。
- 模型无关的媒体、Tensor、批处理和流控抽象。
- MPP、RGA、RKNN、V4L2、ALSA、Streaming和OpenCL通用插件。
- CMake、容器、CI、诊断工具和框架文档。

不应进入主仓库：

- YOLO、ASR或其他具体模型语义及后处理。
- 模型文件、标签、测试视频和业务数据。
- 产品UI、Web服务、单一客户流程或固定设备编号。
- 默认`privileged`、挂载整个`/dev`等量产不安全配置。

## Pull Request要求

PR描述至少包含：

```text
问题：为什么需要修改
方案：公共行为、线程模型或所有权如何变化
风险：兼容性、性能、资源和停止路径风险
验证：实际执行的命令与结果
未验证：板端、驱动、设备或环境限制
文档：更新了哪些设计、参考或ADR
```

禁止使用“测试通过”代替具体结果。硬件Mock通过不能表述为真实RK3588/RK3576硬件验证通过。

## 提交信息

推荐使用简短的祈使句主题，并用可选作用域说明模块：

```text
core: pair DMA-BUF CPU access synchronization
rockchip: handle MPP decoder info-change buffers
tests: cover source drain timeout recovery
docs: document contributor debugging workflow
```

避免把格式化、无关重命名、依赖升级和功能修改混入同一提交。

## 兼容性与ADR

- 修改公共C++ API、YAML schema或插件ABI时，必须说明迁移方式。
- 插件ABI不兼容时升级入口版本，不能静默加载旧插件。
- 改变核心依赖方向、线程模型、所有权或部署安全策略时新增ADR。
- ADR只追加或标记废弃，不通过重写历史掩盖旧决策。

安全问题或可能造成设备损坏、数据泄露的问题不应先公开提交可利用细节，应先联系仓库维护者。
