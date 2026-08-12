# ADR-0006: Docker设备发现与CDI

- 状态：Accepted
- 日期：2026-08-11

## 背景

Rockchip相机和加速器设备节点会随板型、驱动和相机数量变化。固定映射`/dev/video0`不能支持多相机，生产默认挂载整个`/dev`和`privileged`权限过大。

## 决策

公开单一`docker/rkavp-docker`入口。默认优先使用`rockchip.com/*` CDI设备；没有CDI时动态生成精确Compose设备映射；只有`--debug`允许privileged和整个`/dev`。

## 后果

- 普通用户不需要理解Compose叠加细节。
- 生产保持最小权限并支持多相机。
- 量产主机需要维护CDI刷新或允许启动脚本扫描设备。
- 业务YAML选择设备，部署层只授予设备可见性。

## 替代方案

固定设备编号或默认`--privileged -v /dev:/dev`。前者不稳定，后者不满足生产安全目标。
