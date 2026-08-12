# 架构决策记录

ADR记录已经影响公共架构、依赖方向或交付方式的重要决策。教程说明“怎么用”，设计文档说明“系统是什么”，ADR说明“为什么当时这样决定”。

## 状态

- Proposed：讨论中。
- Accepted：当前采用。
- Superseded：已被新ADR替代，但文件保留。
- Deprecated：不建议新代码继续采用。

已接受的决策：

- [ADR-0001 日志门面](0001-logging.md)
- [ADR-0002 CMake与YAML](0002-cmake-yaml.md)
- [ADR-0003 平台无关核心与插件](0003-plugin-architecture.md)
- [ADR-0004 Python只做控制面](0004-python-control-plane.md)
- [ADR-0005 每Edge独立有界队列](0005-bounded-edge-queues.md)
- [ADR-0006 Docker设备发现与CDI](0006-container-device-discovery.md)

ADR接受后不重写历史理由。决策改变时新增ADR，并在旧文件中标记`Superseded by ADR-xxxx`。
