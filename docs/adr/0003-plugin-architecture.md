# ADR-0003: 平台无关核心与插件

- 状态：Accepted
- 日期：2026-08-11

## 背景

MPP、RGA、RKNN、ALSA、ZLMediaKit和OpenCL只在部分平台可用。把它们链接进核心会阻止x86测试，并扩大公共ABI。

## 决策

`librkavp_core`只包含通用Packet、Buffer、Graph、调度和服务。硬件能力通过可选模块和版本化入口`rkavp_plugin_init_v2`注册节点。

核心公共入口为`rkavp/core.hpp`；Rockchip、Audio、Streaming和OpenCL分别使用独立聚合头。源码实现位于`src/modules/<module>`，每个模块维护自己的CMake目标。

## 后果

- 核心可以在x86构建和Mock测试。
- 业务只链接`RKAVP::*`并按需加载插件。
- 插件API变化必须显式升级入口并拒绝不兼容版本。当前接口传递C++类型，要求插件与Runtime使用兼容工具链和同版本SDK，不宣称跨编译器稳定ABI。
- 硬件能力探测和运行库打包由部署层承担。

## 替代方案

单一大库或在核心中使用大量条件编译。它们会弱化依赖边界并增加组合测试数量。
