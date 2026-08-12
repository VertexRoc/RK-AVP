# 日志与可观测性

## API

业务和框架统一使用：

```cpp
RKAVP_LOG(Info) << "graph started";
RKAVP_LOG(Warning) << "input queue dropped a frame";
RKAVP_LOG(Error) << status.message();
```

禁止在业务代码直接使用 `ABSL_LOG`、`LOG`、`std::cerr` 或厂商 SDK 的日志宏。这样可以在不修改节点代码的情况下更换 sink 或日志库。

## 级别

| 级别 | 用途 |
|---|---|
| Trace | 高频调试信息，发布环境默认关闭 |
| Debug | 配置、状态转换和低频诊断 |
| Info | 图启动、停止、设备打开等边界事件 |
| Warning | 可恢复降级、丢帧、重连和 overrun |
| Error | 当前操作失败，需要向上返回 Status |
| Fatal | 进程无法继续，只用于不可恢复的框架不变量 |

`RKAVP_LOG_LEVEL` 可设置 `trace/debug/info/warning/error/fatal`。

## 上下文

GraphRunner worker 自动注入 graph 和 node。采集或推理节点应补充 source 和 frame ID：

```cpp
rkavp::ScopedLogContext context({"live", "transform", "camera0", frame_id});
RKAVP_LOG(Debug) << "inference complete";
```

默认文本输出包含 UTC 时间、级别、线程、图、节点、源、帧、文件和行号。`SetLogSink` 可以接入 journald、轮转文件或集中日志系统。

## 使用规则

- 底层函数返回 Status，上层边界只记录一次，避免重复日志。
- 不记录音视频 payload、模型数据、密钥或 RTSP 凭证。
- 高频路径只记录计数器或采样日志，不逐帧输出 Info。
- 丢帧、ALSA overrun、RTSP 重连和硬件 fallback 同时更新 Metrics。
- Fatal 会终止进程，普通设备或模型错误不得使用 Fatal。

Google 库选择和 RK-AVP 门面决策见 [ADR-0001](../adr/0001-logging.md)。
