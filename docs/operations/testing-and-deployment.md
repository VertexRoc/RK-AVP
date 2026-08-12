# 测试与发布门禁

部署命令只在[部署教程](deployment-guide.md)维护，CI操作只在
[CI教程](ci-guide.md)维护，量产验收只在[量产教程](production-guide.md)维护。
本文仅定义测试分层、CTest标签和发布门禁，不重复部署步骤。

## 验证层次

```text
board endurance and performance
real RK3588/RK3576 hardware integration
installed-package and pure-memory graph integration
core and backend-contract unit tests
```

主机测试覆盖 Timestamp、Packet、Buffer、MediaCaps、YAML、Subgraph、Graph、Executor、Edge Queue、输入同步、Side Packet、Service、版本化插件API、日志、Metrics 和 Trace。MPP/RGA/RKNN/V4L2/ALSA/Streaming 通过接口 Mock 验证参数、EOS、错误和取消，不把 Mock 结果冒充硬件验证。

Python测试遵循MediaPipe式“Python控制面、C++数据面”边界，覆盖结构化Packet、时间戳、Side Packet、Timestamp Bound、图生命周期、取消、多输出观察者、慢回调有界队列、异常隔离、Metrics和Chrome Trace导出。Service注册、DMA-BUF和逐像素处理保留在C++插件层，避免GIL进入媒体热路径。

CTest 标签为 `unit`、`integration`、`python`、`hardware` 和 `performance`。Pull Request 运行 Debug、Release、ASan/UBSan、TSan、Python、AArch64 Rockchip 交叉构建和 Docker `ci` target。

## 镜像门禁

- `ci`：构建主机核心、Python 绑定并执行测试。
- `sdk-arm64`：包含 AArch64 工具链和完整框架源码，供外部插件开发。
- `devel-rk3588/devel-rk3576`：Runtime加头文件、CMake包和插件构建工具。
- `runtime-rk3588/runtime-rk3576`：仅框架、插件、运行库、许可证和兼容清单。

Runtime 镜像不包含模型、媒体文件和业务图。应用通过只读 volume 注入 YAML/资源，并以 `--plugin` 或 `RKAVP_PLUGIN_PATH` 加载插件。安装树使用 `$ORIGIN` RPATH，厂商 `.so` 与框架插件位于同一 lib 目录。

镜像门禁必须构建`ci`以及两个ARM64 Runtime target，并验证Compose的普通、CDI和Debug overlay。设备发现与具体启动命令见部署教程；本门禁只要求生产配置保持精确设备授权、只读根文件系统、`cap_drop: ALL`和`no-new-privileges`，Debug overlay不得进入量产配置。

## 板端门禁

自托管 Runner 分别带 `rk3588`、`rk3576` label，验证：

- V4L2 MMAP/DMA-BUF 采集和可取消停止。
- MPP 硬件解码、编码、info-change 和 EOS drain。
- RGA crop/resize/format/composite 和异步 Fence。
- RKNN 多输入输出、动态 Shape、Tensor memory 复用和性能查询。
- ALSA 格式协商、时间戳和 overrun 恢复。
- Streaming 断线重连、关键帧请求和网络线程隔离。
- DMA-BUF 路径以及至少 30 分钟的持续运行。

板端 `hardware` 标签会验证 SoC、设备可见性、所有后端插件API，并通过 `RKAVP_TEST_CAMERA_0/1` 实际打开两路相机、取得Packet和执行可取消停止；`performance` 标签默认持续运行1800秒。双相机采集仍不等同于完整MPP/RGA/RKNN业务链路，量产门禁还必须配置编码样本、RKNN模型和期望输出，运行端到端图测试。报告必须记录SoC、内核、MPP/RGA版本、RKNN Runtime可加载状态、RKNPU驱动和固件版本。fd增长、线程泄漏、停止超时、死锁或sanitizer错误均为发布阻断。
