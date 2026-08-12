# CMake选项参考

## 依赖

| 选项 | 默认 | 作用 |
|---|---:|---|
| `RKAVP_FETCH_DEPS` | ON | 下载缺少的固定版本源码到`3rdparty/src` |
| `RKAVP_DEPS_SOURCE_DIR` | `3rdparty/src` | 第三方源码目录；生成文件仍位于当前构建目录 |
| `RKAVP_FORCE_FETCH_DEPS` | OFF | 忽略系统包并强制使用下载依赖 |

## 模块

| 选项 | 默认 | 作用 |
|---|---:|---|
| `RKAVP_ENABLE_ROCKCHIP` | OFF | 构建MPP/RGA/RKNN/V4L2插件 |
| `RKAVP_ENABLE_AUDIO` | OFF | 构建ALSA插件 |
| `RKAVP_ENABLE_STREAMING` | OFF | 构建ZLMediaKit插件 |
| `RKAVP_ENABLE_OPENCL` | OFF | 构建OpenCL插件 |
| `RKAVP_ENABLE_PYTHON` | OFF | 构建Python绑定 |

## 测试

| 选项 | 默认 | 作用 |
|---|---:|---|
| `RKAVP_BUILD_TESTS` | ON | 构建主机测试 |
| `RKAVP_BUILD_HARDWARE_TESTS` | OFF | 构建真实Rockchip设备测试 |
| `RKAVP_BUILD_EXAMPLES` | OFF | 构建框架示例 |
| `RKAVP_ENABLE_SANITIZERS` | OFF | ASan与UBSan |
| `RKAVP_ENABLE_THREAD_SANITIZER` | OFF | TSan |
| `RKAVP_ENABLE_COVERAGE` | OFF | gcov覆盖率插桩 |

ASan/UBSan与TSan不能同时启用。

## 目标SoC

```text
RKAVP_TARGET_SOC=host|rk3588|rk3576
```

主机构建：

```bash
cmake --preset host-debug
cmake --build --preset host-debug --parallel
```

Rockchip交叉构建：

```bash
cmake -S . -B build/rk3588 \
  -DCMAKE_TOOLCHAIN_FILE=cmake/toolchains/aarch64-linux-gnu.cmake \
  -DCMAKE_BUILD_TYPE=Release \
  -DRKAVP_TARGET_SOC=rk3588 \
  -DRKAVP_ENABLE_ROCKCHIP=ON \
  -DRKAVP_FETCH_DEPS=ON
```

全功能板端构建：

```bash
cmake -S . -B build/hardware \
  -DCMAKE_BUILD_TYPE=Release \
  -DRKAVP_TARGET_SOC=rk3588 \
  -DRKAVP_ENABLE_ROCKCHIP=ON \
  -DRKAVP_ENABLE_AUDIO=ON \
  -DRKAVP_ENABLE_STREAMING=ON \
  -DRKAVP_BUILD_TESTS=ON \
  -DRKAVP_BUILD_HARDWARE_TESTS=ON
```
