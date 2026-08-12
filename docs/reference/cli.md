# 命令行参考

## rkavp-run

```text
rkavp-run doctor
rkavp-run plugins [--plugin library.so]
rkavp-run validate --graph file.yaml [--plugin library.so]
rkavp-run inspect  --graph file.yaml [--plugin library.so]
rkavp-run run      --graph file.yaml [--plugin library.so]
rkavp-run trace    --graph file.yaml --output trace.json [--plugin library.so]
```

- `doctor`：检查SoC、设备和运行库。
- `plugins`：加载插件并列出注册节点类型。
- `validate`：加载和验证图，不启动节点。
- `inspect`：输出展开和验证后的图信息。
- `run`：运行图，SIGINT/SIGTERM触发停止。
- `trace`：运行图并在停止后导出Chrome Trace JSON。

插件通过重复`--plugin`、可执行文件相邻目录或`RKAVP_PLUGIN_PATH`加载。

## rkavp-ctk

```text
rkavp-ctk discover
rkavp-ctk doctor
rkavp-ctk compatibility
rkavp-ctk cdi generate --output directory
```

- `discover`：列出设备分类和宿主机/容器路径。
- `doctor`：输出平台能力。
- `compatibility`：输出框架、SoC和MPP/RGA/RKNN版本信息。
- `cdi generate`：生成`rockchip.com/*` CDI设备规范。

## Docker入口

```text
docker/rkavp-docker build [rk3588|rk3576] [image]
docker/rkavp-docker run [rk3588|rk3576] [--cdi|--debug] [rkavp-run参数]
docker/rkavp-docker validate
```

默认`run`自动选择CDI或动态精确设备映射。`--debug`才启用privileged和整个`/dev`挂载。

## 环境变量

| 变量 | 作用 |
|---|---|
| `RKAVP_LOG_LEVEL` | `trace/debug/info/warning/error/fatal` |
| `RKAVP_PLUGIN_PATH` | 冒号分隔插件目录或文件路径 |
| `RKAVP_IMAGE` | Docker入口使用的Runtime镜像 |
| `RKAVP_SOC` | Compose传入的目标SoC |
| `RKNN_LOG_LEVEL` | RKNN Runtime日志级别 |
