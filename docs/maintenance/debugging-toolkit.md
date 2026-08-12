# 代码排查工具

排查时先缩小问题层次：配置解析、Graph运行时、Node实现、硬件backend、驱动设备或容器授权。不要在未确认层次前同时修改多个模块。

## 构建与测试定位

显示失败测试完整输出：

```bash
ctest --test-dir build/host-debug-make --output-on-failure
ctest --test-dir build/host-debug-make -N
ctest --test-dir build/host-debug-make -R 'GraphRunner|DmaBuffer' -V
```

直接运行GoogleTest：

```bash
build/host-debug-make/tests/rkavp_unit_tests \
  --gtest_filter='DmaBufferTest.*' --gtest_repeat=100 --gtest_break_on_failure
```

检查CMake实际值和链接关系：

```bash
cmake -LAH -N build/host-debug-make
cmake --build build/host-debug-make --verbose
ldd build/host-debug-make/librkavp_core.so
readelf -d build/host-debug-make/librkavp_core.so
```

Core中出现Rockchip、ALSA、ZLMediaKit或OpenCL库属于架构错误。

## 调试器和系统调用

```bash
gdb --args build/host-debug-make/rkavp-run run --graph graph.yaml
strace -ff -tt -o build/rkavp.strace \
  build/host-debug-make/rkavp-run run --graph graph.yaml
lsof -p <pid>
```

常用`strace`过滤：

```bash
strace -ff -e trace=openat,close,ioctl,poll,ppoll,futex,mmap,munmap \
  -o build/rkavp-io.strace <command>
```

`futex`长期等待通常需要结合线程栈判断，不能直接认定为死锁。

## 内存、fd和并发

ASan/UBSan：

```bash
cmake -S . -B build/sanitizers -G Ninja \
  -DRKAVP_BUILD_TESTS=ON -DRKAVP_ENABLE_SANITIZERS=ON
cmake --build build/sanitizers --parallel
ASAN_OPTIONS=detect_leaks=1:strict_string_checks=1 \
  ctest --test-dir build/sanitizers --output-on-failure
```

WSL或受ptrace约束的环境可能无法运行LeakSanitizer。可以临时使用`detect_leaks=0`定位Address/Undefined问题，但发布门禁仍应在原生Linux执行LeakSanitizer。

TSan：

```bash
cmake -S . -B build/tsan -G Ninja \
  -DRKAVP_BUILD_TESTS=ON -DRKAVP_ENABLE_THREAD_SANITIZER=ON
cmake --build build/tsan --parallel
ctest --test-dir build/tsan --output-on-failure
```

WSL GCC 11出现`unexpected memory mapping`表示TSan运行时未启动，不能记为测试通过或代码竞争报告。改用GitHub Ubuntu Runner、原生Linux或兼容Clang环境。

观察fd和线程是否增长：

```bash
ls /proc/<pid>/fd | wc -l
ls /proc/<pid>/task | wc -l
cat /proc/<pid>/status | rg 'Threads|VmRSS'
```

长稳测试应周期采样，而不是只比较启动和退出两个点。

## Graph运行时

```bash
rkavp-run validate --graph graph.yaml
rkavp-run inspect --graph graph.yaml
rkavp-run trace --graph graph.yaml --output trace.json
```

重点检查：

- Timestamp是否单调，Bound是否倒退。
- Edge队列深度和丢包是否持续增长。
- 慢Node是否与主媒体链路共享Executor。
- `WaitUntilDone`是在等待输入关闭、Source EOS还是Executor drain。
- Error callback和Observer是否执行阻塞业务。

Chrome Trace JSON可在浏览器性能工具或Perfetto中查看Node处理和队列等待时间。

## Rockchip板端

基础设备检查：

```bash
rkavp-run doctor
rkavp-ctk discover
cat /proc/device-tree/compatible | tr '\0' '\n'
ls -l /dev/video* /dev/media* /dev/v4l-subdev* /dev/rga /dev/rknpu* /dev/dma_heap/* 2>/dev/null
v4l2-ctl --list-devices
arecord -l
```

驱动日志：

```bash
dmesg --follow | rg -i 'rga|mpp|vpu|rknpu|iommu|dma|v4l2'
```

DMA-BUF问题应同时记录fd、plane、offset、stride、format、import次数、Fence状态和CPU访问START/END。只比较图像结果不能证明零拷贝链路成立。

## Docker和设备授权

```bash
docker info
docker/rkavp-docker validate
docker run --rm rkavp:ci rkavp-run doctor
```

容器内排查：

```bash
docker inspect <container>
docker exec <container> sh -lc 'ls -l /dev; ldconfig -p; rkavp-run doctor'
```

优先检查精确设备映射、CDI、组权限和运行库。不要把改成`--privileged -v /dev:/dev`作为量产修复；它只能作为临时诊断对照。

## 静态检查与代码搜索

```bash
rg -n 'TODO|FIXME|HACK' include src tests
rg -n 'detach\(|new |delete |dup\(|close\(|mmap\(|munmap\(' include src
rg -n 'MPP|RGA|RKNN|ALSA|OpenCL|ZLMedia' src/core include/rkavp
```

搜索结果是审核入口，不代表每处都是错误。确认所有权和依赖上下文后再修改。

## 问题报告模板

提交问题时至少提供：

```text
版本/提交：
平台：x86、RK3588或RK3576
系统与编译器：
CMake选项：
复现Graph/YAML：
复现步骤：
期望与实际行为：
最小日志和Status：
doctor输出：
Sanitizer/线程/fd结果：
容器镜像Digest和设备授权：
```

删除模型、媒体、客户地址、令牌和设备序列号等敏感数据后再附日志。
