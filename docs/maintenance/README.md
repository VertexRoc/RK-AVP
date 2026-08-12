# 维护者手册

本目录描述如何修改、审核和排查RK-AVP。使用框架请从[框架入门](../getting-started/framework.md)开始；部署和量产分别参考[部署教程](../operations/deployment-guide.md)和[量产教程](../operations/production-guide.md)。

## 阅读顺序

1. [开发环境](development-environment.md)：WSL、Ubuntu、Docker和交叉编译环境。
2. [代码与评审规范](coding-and-review.md)：变更流程、边界、测试和PR门禁。
3. [排查工具](debugging-toolkit.md)：编译、内存、并发、fd、性能和板端问题。
4. [CI教程](../operations/ci-guide.md)：GitHub Actions及本地复现命令。

## 文档职责

| 内容 | 维护位置 |
|---|---|
| 贡献入口和PR要求 | 根目录`CONTRIBUTING.md` |
| 环境安装和工具版本 | `maintenance/development-environment.md` |
| 代码、测试、评审规范 | `maintenance/coding-and-review.md` |
| 故障定位命令 | `maintenance/debugging-toolkit.md` |
| 自动化工作流 | `operations/ci-guide.md` |
| 容器和板端部署 | `operations/deployment-guide.md` |
| 稳定接口和参数 | `reference/` |
| 架构原因和取舍 | `design/`及`adr/` |

命令或接口变化时只更新其权威位置，其他文档使用链接，避免多份说明逐渐不一致。
