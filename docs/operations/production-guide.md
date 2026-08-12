# 量产教程

量产不是“Docker能启动”。它要求硬件兼容、资源边界、升级回滚、可观测性和长时间稳定性都可以重复验证。本章给出RK-AVP从开发图进入设备产品的建议门禁。

## 量产分层

```text
immutable framework runtime
  + pinned Rockchip libraries
  + RK-AVP plugins
  + compatibility manifest

versioned application layer
  + business plugins
  + graph YAML
  + models and resources
  + product configuration

device configuration
  + kernel and drivers
  + CDI device policy
  + secrets and certificates
  + systemd/container supervisor
```

三层应分别版本化。不要把板端驱动、框架Runtime和某个模型业务打包成无法独立升级的单一目录。

## 固定版本

量产发布必须固定：

- RK-AVP Git commit和版本。
- 插件API版本、编译器和C++标准库兼容性。
- MPP、librga、RKNN Runtime和ZLMediaKit版本。
- 基础镜像Digest。
- 业务插件、YAML和模型哈希。
- 内核、RKNPU驱动和固件版本。

`deploy/compatibility.yaml`记录框架已构建的第三方版本，但实际板端驱动兼容结果还应进入产品发布清单。

## 启动前检查

设备启动流程建议为：

```text
driver ready
  -> refresh CDI
  -> rkavp-run doctor
  -> validate graph and plugins
  -> start application graph
  -> publish readiness
```

`doctor`失败时不要反复重启业务容器，应进入受控退避并上报设备、驱动和运行库状态。

## 图设计门禁

每张量产图至少明确：

- 每个Executor的线程数和队列容量。
- 每条关键Edge的容量与丢帧策略。
- Source断线、EOS和重连策略。
- 慢推理分支的FlowLimiter。
- 多输入节点的时间戳策略。
- 停止和排空超时。
- Buffer Pool容量和耗尽行为。
- 关键指标和告警阈值。

实时视频通常使用小容量`drop_oldest`；录像、音频文件和控制命令可能要求`block`。不要全局采用同一种队列策略。

## 多相机与动态Source

设备路径优先使用`/dev/v4l/by-id`或`by-path`，不要依赖枚举顺序稳定。动态Source只允许使用YAML预声明槽位，避免运行时任意修改Node和Edge导致不可验证状态。

Source删除流程必须完成：

```text
stop input -> EOS -> drain -> release buffers/fd/context
```

单路相机断开不得阻塞其他Source、主码流或控制Executor。

## 零拷贝验收

不能仅凭代码中出现DMA-BUF就宣称零拷贝。板端应记录：

- V4L2或MPP输出fd。
- RGA import handle及缓存命中次数。
- RKNN输入内存绑定方式。
- CPU map和cache sync次数。
- Buffer Pool申请、复用和峰值。
- Fence等待时间。

验收目标是MPP、RGA和RKNN之间不复制媒体载荷到普通CPU Buffer。元数据和小型控制结构的复制不属于媒体载荷复制。

## 资源预算

每个产品图都应形成预算表：

| 资源 | 需要记录的指标 |
|---|---|
| CPU | 平均、P95、峰值和单Executor占用 |
| 内存 | RSS、Buffer Pool、MPP/RKNN内存和泄漏趋势 |
| NPU | 单次执行、队列等待、core mask和利用率 |
| RGA | 提交速率、失败、Fence等待和格式转换耗时 |
| 视频 | FPS、PTS抖动、丢帧、编码码率和关键帧间隔 |
| 网络 | 重连、jitter、late packet、发送队列和带宽 |
| fd/线程 | 启动基线、运行峰值和停止后回归值 |

性能测试必须包含预处理、调度、推理、后处理和编码的端到端延迟，不能只引用RKNN模型执行时间。

## 稳定性门禁

建议最低执行：

- 四路输入连续运行30分钟作为开发门禁。
- 目标产品负载连续运行24小时作为发布门禁。
- Source增删100次。
- 网络断开和恢复100次。
- 相机拔插、NPU失败和磁盘写满故障注入。
- 容器Stop后线程和fd在超时内回到基线。
- 队列深度始终不超过配置。
- 单路故障不影响其他Source。

当前仓库的`tests/performance/test_endurance.cpp`只验证纯内存Passthrough图，不等价于上述真实硬件长稳测试。

## 可观测性

生产环境应采集：

- 节点处理延迟和错误数。
- Edge队列深度、等待时间和丢包数。
- Source状态和最后PTS。
- Executor活动任务、排队和饥饿。
- Buffer Pool占用。
- RTSP重连、ALSA overrun、MPP/RGA/RKNN错误。

日志自动带Graph、Node、Executor、Stream、PTS、Frame ID和线程上下文。高频路径使用Metrics和采样日志，不逐帧输出Info。

Trace只在诊断窗口开启并导出Chrome Trace JSON，避免长期高频跟踪占用存储。

## 进程监督

建议由systemd、Docker Compose或设备管理代理监督容器：

- 使用健康检查区分进程存活与图可用。
- 设置有上限的重启退避。
- Stop时先调用图Cancel/Stop，再由容器超时发送强制终止。
- 日志交给journald或轮转系统，不无限写容器层。
- 只读根文件系统，持久数据写入明确Volume。

## 升级和回滚

使用镜像Digest部署：

```text
ghcr.io/example/rk-avp@sha256:...
```

升级流程：

1. 下载新镜像但不替换当前实例。
2. 校验签名、SBOM和兼容清单。
3. 在预发布设备执行doctor和最小硬件图。
4. 小批量灰度。
5. 监控错误率、队列、fd、线程和温度。
6. 扩大范围或回滚到上一Digest。

YAML、模型和插件必须与Runtime一起形成原子版本，避免只升级其中一个造成插件API或Tensor描述不匹配。

## 安全要求

- 生产禁止`--debug`和`privileged`。
- 精确授权设备，优先CDI。
- 容器`cap_drop: ALL`与`no-new-privileges`。
- 密钥不进入镜像、Git、YAML和日志。
- RTSP输入和管理API限制网络访问范围。
- 第三方依赖保留许可证、SBOM和漏洞扫描结果。
- 自托管CI Runner与生产网络隔离。

## 发布验收清单

- [ ] x86 unit/integration/Python全部通过。
- [ ] ASan/UBSan和原生Linux TSan通过。
- [ ] RK3588和RK3576交叉编译通过。
- [ ] 目标板真实V4L2、MPP、RGA、RKNN、ALSA、Streaming通过。
- [ ] DMA-BUF路径有fd/import/CPU map证据。
- [ ] 24小时目标负载无fd、线程和内存增长。
- [ ] 单路故障隔离和重连通过。
- [ ] Runtime与业务镜像使用不可变Digest。
- [ ] SBOM、许可证和兼容矩阵归档。
- [ ] 升级和回滚演练通过。

没有完成真实硬件和长稳门禁时，版本可以作为开发预览发布，但不应标记为量产就绪。
