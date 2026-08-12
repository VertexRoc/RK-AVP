# 开发环境

## 支持环境

推荐使用Ubuntu 22.04或Ubuntu 22.04 WSL2进行x86开发。RK3588/RK3576功能验证必须在对应开发板或自托管ARM64 Runner执行；交叉编译只能证明可以生成目标产物。

基础工具：

```bash
sudo apt-get update
sudo apt-get install -y \
  build-essential cmake ninja-build git pkg-config \
  libyaml-cpp-dev libgtest-dev \
  clang-format clang-tidy lcov \
  python3-dev python3-pytest pybind11-dev \
  gcc-aarch64-linux-gnu g++-aarch64-linux-gnu
```

可选排查工具：

```bash
sudo apt-get install -y \
  gdb strace lsof valgrind linux-tools-common \
  v4l-utils alsa-utils jq
```

OpenCL插件实编译还需要平台OpenCL开发包和厂商ICD。Ubuntu主机可安装
`ocl-icd-opencl-dev opencl-headers`；板端应优先使用SoC厂商提供的SDK。只有
`libOpenCL.so`运行库而没有头文件时，CMake无法启用该模块。

## 获取依赖

默认主机构建使用系统yaml-cpp和GoogleTest，不访问网络：

```bash
cmake --preset host-debug
```

需要拉取固定版本源码时：

```bash
cmake -S . -B build/fetch -G Ninja \
  -DRKAVP_FETCH_DEPS=ON \
  -DRKAVP_BUILD_TESTS=ON
cmake --build build/fetch --target rkavp-prefetch
```

第三方源码必须位于`3rdparty/src`，生成文件位于具体`build/`目录。不要把第三方构建产物提交到仓库。

网络代理由宿主机或Docker Desktop统一配置。不要把个人IP、端口、用户名或密码写入CMake、Dockerfile、Git配置示例或CI。

## 主机构建

```bash
cmake --preset host-debug
cmake --build --preset host-debug --parallel
ctest --preset host-debug
```

切换编译器、Generator、Toolchain或Sanitizer时使用新的构建目录，不复用旧CMakeCache：

```bash
cmake -S . -B build/clang -G Ninja \
  -DCMAKE_C_COMPILER=clang \
  -DCMAKE_CXX_COMPILER=clang++ \
  -DRKAVP_BUILD_TESTS=ON \
  -DRKAVP_FETCH_DEPS=OFF
```

## Python

```bash
cmake -S . -B build/python -G Ninja \
  -DRKAVP_ENABLE_PYTHON=ON \
  -DRKAVP_BUILD_TESTS=ON
cmake --build build/python --parallel
ctest --test-dir build/python -L python --output-on-failure
```

直接运行pytest时必须让Python找到构建后的模块：

```bash
PYTHONPATH=build/python/python python3 -m pytest tests/python -q
```

## RK3588和RK3576交叉编译

```bash
cmake --preset rk3588-release
cmake --build --preset rk3588-release --parallel

cmake --preset rk3576-release
cmake --build --preset rk3576-release --parallel
```

依赖源码应先预取。发布前还需在真实板端执行`hardware`和`performance`标签测试。

## Docker

确保Docker Desktop已启动并且当前用户能访问daemon：

```bash
docker info
docker build --target ci -t rkavp:ci .
```

出现`permission denied ... docker.sock`表示daemon权限或当前执行环境隔离，不是Dockerfile编译错误。出现镜像仓库超时应检查Docker Desktop代理，因为Docker daemon不一定继承WSL shell环境变量。

完整部署方式见[部署教程](../operations/deployment-guide.md)。
