# ADR-0002: CMake与YAML

- 状态：Accepted
- 日期：2026-08-11

## 背景

RK-AVP需要被传统C++项目、Rockchip SDK和外部插件直接消费。MediaPipe使用Bazel和Protobuf，但完整复制会扩大工具链和生成代码依赖。

## 决策

工程使用C++17与CMake，安装后导出`RKAVP::*`目标；图配置使用带版本号的YAML和结构化`ConfigValue`。

## 后果

- 外部工程可以使用`find_package(RKAVP)`。
- 板端人员可以直接阅读和修改图。
- 失去部分Proto和MediaPipe类型化Builder的编译期检查。
- YAML兼容必须由版本号、Schema文档和验证器维护。

## 替代方案

完整采用Bazel/Protobuf，或自定义二进制配置格式。两者均不符合当前轻量和CMake生态目标。
