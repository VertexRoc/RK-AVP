# CI教程

RK-AVP把主机正确性、交叉编译、真实硬件和镜像发布拆成独立工作流。这样普通Pull Request不依赖开发板，硬件结果也不会被Mock测试冒充。

## 工作流总览

```text
Pull Request
  |-- CI                 x86 build, unit, integration, Python, sanitizers, Docker
  |-- Cross Build        RK3588/RK3576 AArch64 compile and install

Manual / protected branch
  |-- Rockchip Hardware  self-hosted RK3588/RK3576 tests

Version tag
  `-- Release Containers ARM64 runtime/devel/sdk images -> GHCR
```

对应文件：

- `.github/workflows/ci.yml`
- `.github/workflows/cross-build.yml`
- `.github/workflows/hardware.yml`
- `.github/workflows/release-containers.yml`

## 本地复现主机CI

安装依赖：

```bash
sudo apt-get update
sudo apt-get install -y cmake ninja-build libyaml-cpp-dev libgtest-dev \
  pybind11-dev python3-dev python3-pytest
```

Debug和普通测试：

```bash
cmake --preset host-debug
cmake --build --preset host-debug --parallel
ctest --preset host-debug
```

Release：

```bash
cmake -S . -B build/release -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DRKAVP_BUILD_TESTS=ON \
  -DRKAVP_FETCH_DEPS=OFF
cmake --build build/release --parallel
ctest --test-dir build/release --output-on-failure
```

Python：

```bash
cmake -S . -B build/python -G Ninja \
  -DCMAKE_BUILD_TYPE=Debug \
  -DRKAVP_BUILD_TESTS=ON \
  -DRKAVP_ENABLE_PYTHON=ON
cmake --build build/python --parallel
ctest --test-dir build/python -L python --output-on-failure
```

## Sanitizer

ASan和UBSan检查越界、use-after-free和未定义行为：

```bash
cmake -S . -B build/sanitizers -G Ninja \
  -DCMAKE_BUILD_TYPE=Debug \
  -DRKAVP_BUILD_TESTS=ON \
  -DRKAVP_ENABLE_SANITIZERS=ON
cmake --build build/sanitizers --parallel
ASAN_OPTIONS=detect_leaks=1:strict_string_checks=1 \
  ctest --test-dir build/sanitizers --output-on-failure
```

TSan检查数据竞争：

```bash
cmake -S . -B build/tsan -G Ninja \
  -DCMAKE_BUILD_TYPE=Debug \
  -DRKAVP_BUILD_TESTS=ON \
  -DRKAVP_ENABLE_THREAD_SANITIZER=ON
cmake --build build/tsan --parallel
ctest --test-dir build/tsan --output-on-failure
```

WSL中的GCC TSan可能因地址空间布局产生不可信结果，发布门禁应使用GitHub Ubuntu Runner或原生Linux，不应通过关闭检查规避问题。

## ARM64交叉编译

```bash
sudo apt-get install -y gcc-aarch64-linux-gnu g++-aarch64-linux-gnu

cmake -S . -B build/rk3588 \
  -DCMAKE_TOOLCHAIN_FILE=cmake/toolchains/aarch64-linux-gnu.cmake \
  -DCMAKE_BUILD_TYPE=Release \
  -DRKAVP_TARGET_SOC=rk3588 \
  -DRKAVP_ENABLE_ROCKCHIP=ON \
  -DRKAVP_FETCH_DEPS=ON
cmake --build build/rk3588 --parallel
cmake --install build/rk3588 --prefix install/rk3588
```

RK3576只需替换`RKAVP_TARGET_SOC`和构建目录。交叉编译证明代码和依赖可以生成ARM64产物，但不能执行硬件功能测试。

## Docker CI

检查Compose并构建CI镜像：

```bash
docker/rkavp-docker validate
docker build --target ci -t rkavp:ci .
```

`ci` target会在容器内重新配置、编译并执行CTest，包括Python测试和安装后消费测试。因此它同时验证Dockerfile和基本工程可重现性。

## 自托管Rockchip Runner

真实硬件工作流要求两类Runner标签：

```text
self-hosted, linux, arm64, rk3588
self-hosted, linux, arm64, rk3576
```

Runner主机应使用专用测试设备，不与生产业务共享。建议为GitHub Runner创建独立用户，并通过udev规则授予所需设备组权限，不长期使用root Runner。

安装Runner后先手动确认：

```bash
uname -m
cat /proc/device-tree/compatible | tr '\0' '\n'
ls -l /dev/video* /dev/media* /dev/rga /dev/rknpu 2>/dev/null
```

`hardware.yml`目前执行设备可见性和插件加载Smoke Test，以及框架内存图的30分钟endurance。真实MPP/RGA/RKNN/V4L2/ALSA/Streaming端到端用例仍需继续补充，不能仅凭现有workflow名称判断量产验证已经完成。

## CTest标签

```bash
ctest --test-dir build/host -L unit
ctest --test-dir build/host -L integration
ctest --test-dir build/python -L python
ctest --test-dir build/hardware -L hardware
ctest --test-dir build/hardware -L performance
```

新增测试必须选择正确标签。依赖真实设备的用例不得放入`unit`或普通`integration`。

## 分支保护建议

Pull Request至少要求：

- host
- sanitizers
- thread-sanitizer
- release
- python
- docker
- RK3588/RK3576 cross build

涉及`src/modules`、Docker Runtime或第三方版本更新的变更，在合并到发布分支前还应要求两块Rockchip板的硬件结果。

## 发布镜像

推送`v*`标签会构建并发布：

```text
rk-avp:<tag>-rk3588
rk-avp:<tag>-rk3576
rk-avp:devel-rk3588
rk-avp:devel-rk3576
rk-avp:sdk-arm64
```

工作流使用`GITHUB_TOKEN`登录GHCR，不需要额外长期密码。仓库必须允许Actions写入Packages。发布镜像包含OCI标签、provenance和SBOM。

推荐发布流程：

1. 更新版本和`deploy/compatibility.yaml`。
2. 主机、交叉编译和硬件门禁全部通过。
3. 创建不可变Git tag。
4. 发布Runtime与Devel镜像。
5. 在干净板机拉取镜像执行部署验收。
6. 记录镜像Digest，而不只记录可变Tag。

## 覆盖率

CI中的`coverage` job使用`RKAVP_ENABLE_COVERAGE`生成lcov报告，只统计`src/core`，并对核心行覆盖率执行80%门禁。`quality` job执行统一clang-format检查和高信号clang-tidy规则；第三方源码不纳入格式、静态分析或覆盖率统计。
