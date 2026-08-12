# Node与插件测试

`RKAVP::Testing`提供轻量的`NodeTestRunner`。它不依赖GoogleTest，外部插件工程可以选择GoogleTest、Catch2或自己的断言工具。

## CMake

安装Runtime、Development和Testing组件后：

```cmake
find_package(RKAVP CONFIG REQUIRED)

add_executable(my_node_test my_node_test.cpp)
target_link_libraries(my_node_test PRIVATE RKAVP::Core RKAVP::Testing)
target_compile_features(my_node_test PRIVATE cxx_std_17)
```

## 单次节点测试

```cpp
#include "rkavp/testing/node_test_runner.hpp"

rkavp::testing::NodeTestRunner runner(std::make_unique<MyNode>());
runner.Configure(rkavp::ConfigValue::Object{{"scale", 2}});
runner.SetInput("in", rkavp::Packet::Make(21));

rkavp::Status status = runner.RunOnce();
auto outputs = runner.Outputs("out");
```

`RunOnce()`依次完成Configure、Open、Start、Process、Stop和Close。需要测试Source节点或多次Process时，应分别调用`Start()`、`Process()`、`Stop()`和`Close()`。

## 注入运行时依赖

Runner可以注入以下NodeContext依赖：

- `SetInput()`：设置下一次Process使用的输入Packet。
- `SetSidePacket()`：设置不可变Side Packet。
- `SetService<T>()`：注册类型安全的Graph Service。
- `SetResourceManager()`：注入模型、配置或其他资源读取实现。
- `metrics()`：读取节点写入的Counter和Gauge。

异步Source节点可在OnStart保存NodeContext并从工作线程发出Packet。`WaitForOutput()`使用有界超时等待输出；`Cancel()`会唤醒等待线程，并让`NodeContext::cancelled()`变为true。

## 插件契约

仓库内的`tests/install-consumer`不是业务模板，而是安装契约测试。它从安装前缀执行`find_package(RKAVP)`，编译外部Node和共享插件，并验证：

- `RKAVP::Core`和`RKAVP::Testing`可以被外部工程链接。
- `rkavp_plugin_init_v2`插件可以注册、创建和卸载Node。
- 旧版插件API被明确拒绝。

具体模型、后处理和产品应用以后放在独立Samples或业务仓库中，框架仓库不维护它们的副本。
