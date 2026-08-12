# 总体架构

RK-AVP 是面向 RK3588、RK3576 的轻量多媒体图框架，不是检测、语音或 Web 业务工程。公共 API 只表达通用媒体、Tensor、资源、调度和生命周期语义。

## 分层

```text
C++ / Python applications and out-of-tree plugins
                         |
                    YAML Graph API
                         |
  +--------------------------------------------------+
  |                  librkavp_core                   |
  | Packet Timestamp ConfigValue TensorSet           |
  | Buffer DmaBuffer BufferPool MediaCaps            |
  | NodeContext Contract Registry Graph Services     |
  | Executors Edge Queues Metrics Trace Logging      |
  +--------------------------------------------------+
              |             |             |
      rkavp_rockchip   rkavp_audio   rkavp_streaming
      V4L2/MPP/RGA/RKNN    ALSA       ZLMediaKit
              |
       optional rkavp_opencl
```

## 仓库布局

```text
include/rkavp + src/core  平台无关框架运行时
src/modules/rockchip      V4L2、MPP、RGA和RKNN插件
src/modules/audio         ALSA插件
src/modules/streaming     ZLMediaKit插件
src/modules/opencl        OpenCL插件
tools/rkavp-run           图验证、检查和运行工具
graphs                    可复用 YAML 图
examples                  最小框架 API 示例
3rdparty/src              固定版本第三方源码，不保存生成文件
deploy                    板端部署输入，不存放业务实现
```

`tools` 不承载业务，`examples` 只演示公共 API。具体视觉、语音和 Web
产品应在仓库外通过 `find_package(RKAVP)` 和版本化插件API使用框架。当前入口传递C++ `NodeRegistry`，因此要求插件与Runtime使用兼容的编译器、C++标准库和同版本SDK；它不是跨编译器稳定C ABI。

公共聚合头同样保持分层：

```text
rkavp/core.hpp       平台无关API
rkavp/rockchip.hpp   V4L2/MPP/RGA/RKNN接口和节点
rkavp/audio.hpp      ALSA接口和节点
rkavp/streaming.hpp  Streaming接口和节点
rkavp/opencl.hpp     OpenCL接口和节点
```

`rkavp/rkavp.hpp`只兼容转发到`core.hpp`，不再隐式引入可选模块。

依赖只能向下。`rkavp_core` 禁止包含或链接 MPP、RGA、RKNN、ALSA、ZLMediaKit 和 OpenCL。固定版本的第三方源码统一进入`3rdparty/src`，各Preset的生成文件和目标文件留在自己的`build/<preset>`，安装后的使用方只依赖`RKAVP::*`目标。

## 核心数据面

- `Packet` 是带 Timestamp 和控制事件的不可变共享载体，扇出只增加引用计数。
- `Buffer` 表达 Host、DMA-BUF、MPP、RKNN 和 OpenCL 内存，plane 明确 fd、offset、stride 和 modifier。
- `TensorSet` 表达多输入、多输出、量化、动态 Shape 和多模态 Tensor，不包含模型业务语义。
- `ConfigValue` 支持 bool、整数、浮点、字符串、列表和对象。
- `SidePacketSet` 在图启动后不可变；`GraphServiceRegistry` 提供进程内服务；`ResourceManager` 隔离资源解析。

## 运行时

节点通过 `NodeContext` 读取输入集、发送输出，并访问 Side Packet、Service、Resource、Metrics 和取消状态。Source Node 可在独立采集线程主动产生 Packet。

每条 Edge 有独立的有界队列、容量、丢弃策略和统计。节点绑定命名 Executor 线程池，输入策略包括：

- `any`：任一输入到达即调度。
- `sync`：按 Timestamp 匹配必需输入。
- `latest`：主输入触发，其他输入使用最近值。

图支持输入/输出流、异步有界观察者、Timestamp bound、EOS、显式 back edge、嵌套 Subgraph、取消和错误回调。`GraphRuntimeInfo`暴露节点状态、Executor排队和饥饿、Stream Bound、最后时间戳、队列深度与丢包。

`FlowLimiter`对推理等慢分支限制在途Packet；`PacketBatch`、`AdaptiveBatch`和`StreamDemux`提供模型无关的多源批处理。动态能力只开放YAML预声明的`source_slots`，不允许任意Node或Edge热修改。

## 扩展边界

- 仓库内硬件适配通过 backend 接口隔离，x86 使用 Fake/Mock 测试。
- 仓库外 C++ 插件导出 `rkavp_plugin_init_v2`；加载器明确拒绝仅提供v1入口的插件。
- Python 只负责控制面、结构化低频 Packet、Metrics 和有界异步回调，不进入逐像素或逐采样主路径。
- OpenCL 插件提供通用 `OpenClKernel` Tensor 节点及 Fence 传播，不在框架内固化分割、合成等业务 Kernel。
- 新节点必须声明端口和 `MediaCaps`，不得创建无界队列或隐式拥有裸 fd。

## 非目标

- 不内置具体模型、后处理、业务数据结构、模型转换和训练代码。
- 不复制 MediaPipe 的 Bazel/Protobuf 体系或完整 API。
- 不成为 GUI、离线剪辑或通用分布式计算框架。
