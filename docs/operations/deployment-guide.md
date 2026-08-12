# 部署教程

本文说明如何把RK-AVP框架部署到RK3588或RK3576。部署对象是框架Runtime和插件，不包含具体模型、业务图或Web服务。

## 部署结构

```text
framework runtime image
  /opt/rkavp/bin          rkavp-run, rkavp-ctk
  /opt/rkavp/lib          core and optional plugins
  /opt/rkavp/share        licenses and compatibility data

application deployment
  graph.yaml
  out-of-tree plugins
  models and resources
  application configuration
```

框架镜像保持稳定，业务镜像或只读Volume在其上增加模型和图。这与MediaPipe把框架和具体Solution分层的思路一致。

## 前置条件

板端至少需要：

- ARM64 Linux和可用的Rockchip驱动。
- Docker Engine与Compose v2。
- 与`deploy/compatibility.yaml`匹配的MPP、RGA和RKNN运行环境。
- 使用相机时存在`/dev/video*`、`/dev/media*`或`/dev/v4l-subdev*`。
- 使用NPU时存在`/dev/rknpu`或平台对应设备。

先在主机执行：

```bash
uname -m
docker version
docker compose version
ls -l /dev/video* /dev/media* /dev/rga /dev/rknpu 2>/dev/null
```

## 构建Runtime镜像

在x86开发机使用Buildx构建ARM64镜像：

```bash
docker/rkavp-docker build rk3588 rkavp:rk3588
docker/rkavp-docker build rk3576 rkavp:rk3576
```

对应Docker target分别为`runtime-rk3588`和`runtime-rk3576`。首次构建会把固定版本源码放入`3rdparty/src`；编译中间文件只存在于当前镜像构建层和CMake构建目录，不与其他平台共享。

CI发布时使用GHCR镜像；本地验证可直接使用`rkavp:<soc>`。

## 第一次启动

在板端运行设备检查：

```bash
RKAVP_IMAGE=rkavp:rk3588 docker/rkavp-docker run rk3588 doctor
```

`doctor`检查：

- SoC compatible信息。
- 相机与Media Controller节点。
- DRM、RGA、MPP和NPU设备。
- ALSA设备。
- MPP、RGA、RKNN Runtime是否可加载。

设备检查通过只是部署前置条件，仍需执行真实图验证硬件功能。

## 部署业务图

业务YAML保存在业务仓库，例如：

```text
/opt/my-app/graphs/my_pipeline.yaml
```

通过`RKAVP_GRAPH_DIR`只读挂载，先验证再运行：

```bash
RKAVP_IMAGE=rkavp:rk3588 RKAVP_GRAPH_DIR=/opt/my-app/graphs \
  docker/rkavp-docker run rk3588 \
  validate --graph /work/graphs/my_pipeline.yaml

RKAVP_IMAGE=rkavp:rk3588 RKAVP_GRAPH_DIR=/opt/my-app/graphs \
  docker/rkavp-docker run rk3588 \
  run --graph /work/graphs/my_pipeline.yaml
```

未设置`RKAVP_GRAPH_DIR`时，入口挂载仓库内`graphs/`，只用于框架示例和部署检查。RK-AVP仓库不保存产品业务图。

生产图应明确Executor、Edge容量和丢帧策略，不依赖默认无意识运行。

## 设备授权模式

统一入口提供三种模式，但普通用户只需执行同一个`run`命令。

### 自动模式

默认模式先查找RK-AVP CDI配置。如果不存在，则扫描当前主机设备并生成临时Compose override。

自动扫描覆盖：

```text
/dev/video* /dev/media* /dev/v4l-subdev* /dev/camera*
/dev/rga /dev/rknpu /dev/mpp_service
/dev/dri/* /dev/dma_heap/* /dev/snd/*
```

因此多相机场景不需要把`video0`或`video1`写死在Compose中。YAML负责选择使用哪个设备，部署层负责授予可见性。

### CDI模式

宿主机安装`rkavp-ctk`后生成CDI：

```bash
sudo install -d /var/run/cdi
sudo rkavp-ctk cdi generate --output /var/run/cdi
docker/rkavp-docker run rk3588 --cdi doctor
```

CDI逻辑设备包括camera、RGA、MPP、RKNN、audio和DRM。量产环境推荐CDI，因为设备授权更稳定，也方便容器编排系统复用。

仓库提供systemd path/service模板，在启动或设备拓扑变化后刷新CDI。安装模板前必须保证`rkavp-ctk`位于模板声明的`/usr/bin/rkavp-ctk`。

### Debug模式

```bash
docker/rkavp-docker run rk3588 --debug doctor
```

Debug模式启用`privileged`并挂载整个`/dev`，只用于定位权限或设备枚举问题，不作为生产启动方式。

## 生产安全默认值

普通Compose默认：

- 只读根文件系统。
- `cap_drop: ALL`。
- `no-new-privileges`。
- 只为`/tmp`和`/run`创建受限tmpfs。
- 精确设备授权，不挂载整个`/dev`。
- 图目录只读挂载。

RTSP凭证、API密钥和设备证书不要写进镜像、YAML或日志。使用Docker Secret、只读文件挂载或设备上的密钥服务。

## 业务镜像

量产应用应继承框架Runtime：

```dockerfile
FROM ghcr.io/example/rk-avp:rk3588

COPY libmy_nodes.so /opt/app/lib/
COPY graphs/ /opt/app/graphs/
COPY models/ /opt/app/models/

ENV RKAVP_PLUGIN_PATH=/opt/rkavp/lib:/opt/app/lib
ENTRYPOINT ["rkavp-run"]
CMD ["run", "--graph", "/opt/app/graphs/main.yaml"]
```

模型和业务插件由业务仓库管理，RK-AVP Runtime升级不应强制修改业务源码。

## 常用诊断

```bash
docker/rkavp-docker validate
docker/rkavp-docker run rk3588 doctor
docker/rkavp-docker run rk3588 plugins --plugin /opt/rkavp/lib/librkavp_rockchip.so
docker/rkavp-docker run rk3588 inspect --graph /work/graphs/my_pipeline.yaml
```

导出Trace时应在业务Compose中增加可写的诊断Volume，再把`trace --output`
指向该Volume。不要写入临时容器的`/tmp`后立即使用`--rm`删除容器。

排障顺序建议：

1. 宿主机确认设备和驱动。
2. `doctor`确认容器可见性和运行库。
3. `plugins`确认插件API兼容性和节点注册。
4. `validate`确认图结构。
5. 运行最小单设备图。
6. 再增加RGA、RKNN、编码和网络分支。

## 当前验证边界

已经验证x86 CI镜像、Compose配置和ARM64交叉编译。真实相机、MPP、RGA、RKNN、ALSA和Streaming仍必须在RK3588/RK3576自托管Runner或目标设备上执行，不能用x86 Mock结果替代。
