# 代码与评审规范

## 核心原则

- C++17为主，沿用现有CMake目标、Status错误模型和命名风格。
- `rkavp_core`不得链接MPP、RGA、RKNN、ALSA、ZLMediaKit或OpenCL。
- Packet载荷默认不可变共享，fan-out不得隐式复制媒体数据。
- 所有队列必须有容量、丢弃策略、统计和可取消关闭路径。
- 阻塞I/O必须能被`Cancel`或`RequestStop`唤醒。
- Python只进入控制面，不进入逐像素或逐采样热路径。
- 公共API保持模型无关，不出现检测框、YOLO、ASR等业务语义。

## C++规则

- 使用RAII表达fd、线程、映射、Fence和设备Context所有权。
- fd复制使用`F_DUPFD_CLOEXEC`或等价方式，并测试析构后的fd状态。
- DMA-BUF CPU访问必须成对调用`BeginCpuAccess`和`EndCpuAccess`。
- 不持有互斥锁调用用户回调、Node实现或可能阻塞的后端API。
- 错误回调和输出观察者不得阻塞Graph Executor。
- 共享状态必须说明由哪个锁保护；简单状态可使用原子变量。
- 生命周期顺序为`Configure -> Open -> Start -> Stop -> Close`，失败按逆序回滚。
- 不使用分离线程逃避生命周期管理；例外必须证明线程不再访问所属对象。

格式由仓库`.clang-format`决定：

```bash
clang-format -i path/to/changed.cpp path/to/changed.hpp
```

静态分析使用构建目录的compile database：

```bash
cmake -S . -B build/tidy -G Ninja \
  -DCMAKE_EXPORT_COMPILE_COMMANDS=ON \
  -DRKAVP_BUILD_TESTS=OFF
clang-tidy -p build/tidy src/core/graph_runner.cpp
```

不要通过大范围`NOLINT`隐藏问题。抑制规则应紧邻代码并说明原因。

## CMake和依赖

- 业务代码只链接`RKAVP::*`命名目标。
- 可选依赖不得泄漏到Core的PUBLIC接口。
- 新第三方依赖必须固定版本、记录许可证并更新`THIRD_PARTY_NOTICES.md`。
- 不使用`file(GLOB)`收集框架源文件，新增文件应显式列出。
- 安装目标必须归入`Runtime`、`Development`、`Testing`、`Documentation`或`Examples`组件。
- 修改导出目标后必须通过安装后`find_package(RKAVP)`消费测试。

## 测试要求

| 修改类型 | 最低测试 |
|---|---|
| Packet、Timestamp、ConfigValue | unit |
| Graph、Executor、队列、生命周期 | unit + integration + ASan/UBSan + TSan |
| Node公共测试API | unit + installed package test |
| Python绑定 | C++测试 + pytest |
| MPP/RGA/RKNN/V4L2/ALSA/Streaming | Mock测试 + ARM64交叉编译 + 对应板端测试 |
| DMA-BUF/fd/线程所有权 | fd基线 + ASan/UBSan + TSan + 板端链路 |
| CMake安装或导出 | installed package test + Docker CI |
| Docker/deploy | 配置校验 + CI镜像 + 目标板doctor |

时间相关测试优先使用`FakeClock`，不能依赖长时间`sleep`。真实硬件测试必须使用`hardware`标签，不得伪装成普通单元测试。

## 文档同步

- 公共API或数据结构：更新头文件注释、入门和相关reference。
- YAML字段：更新schema、解析测试和示例。
- 调度、队列、EOS或生命周期：更新`design/graph-runtime.md`。
- 硬件内存链路：更新`design/hardware-acceleration.md`。
- Docker或CI：更新对应operations文档。
- 新的不可逆架构选择：新增ADR。

## 评审清单

评审者应按以下顺序检查：

1. 边界：这是框架能力还是业务功能？依赖方向是否正确？
2. 正确性：异常、EOS、空输入、超时、取消和重复启动是否明确？
3. 所有权：Packet、Buffer、fd、线程、回调和插件卸载由谁持有？
4. 并发：回调是否在锁内执行？停止是否会自连接或永久等待？
5. 流控：队列是否有界？慢分支是否阻塞无关分支？
6. 硬件：stride、plane、format、Fence及DMA-BUF同步是否完整？
7. 可观测性：错误是否可定位？是否有丢包、延迟或队列指标？
8. 兼容性：C++ API、YAML、插件ABI和安装目标是否变化？
9. 测试：测试是否真正覆盖失败前的行为，而非只覆盖成功路径？
10. 文档：用户和维护者能否从文档复现修改与验证？

## 合并门禁

合并前至少确认：

- clang-format和clang-tidy通过。
- unit、integration、安装后消费和Python相关测试通过。
- ASan/UBSan通过；并发修改在原生Linux或CI取得可信TSan结果。
- RK3588/RK3576交叉编译通过。
- Docker `ci` target通过。
- 硬件相关修改记录真实板端结果或明确列为未验证。
- 没有提交模型、媒体、凭据、构建目录、缓存或生成文件。
