# ADR-0004: Python只做控制面

- 状态：Accepted
- 日期：2026-08-11

## 背景

Python适合业务编排和快速集成，但逐帧DMA-BUF、像素处理和音频采样会引入GIL、复制和对象生命周期风险。

## 决策

Python绑定只暴露图加载、生命周期、动态Source、Metrics、Trace和有界结构化回调。媒体热路径保留在C++节点或插件中。

## 后果

- Python业务仍可控制和观察Graph。
- 慢回调通过独立有界队列隔离。
- 逐像素算法需要C++插件实现。
- Python对象销毁和回调异常不得终止媒体Executor。

## 替代方案

把VideoFrame和DMA-BUF逐帧暴露给Python。该方案不符合实时性和所有权目标。
