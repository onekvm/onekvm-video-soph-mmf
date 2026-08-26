# onekvm-nanokvm-mmf

[English](README.md) | 简体中文

`onekvm-nanokvm-mmf` 是 OneKVM 面向 NanoKVM 的视频与加密硬件后端。
它将 SG2002 多媒体处理链路接入 OneKVM Backend ABI，并将设备相关实现隔离在
独立动态库中，避免 `onekvm-server` 直接依赖 NanoKVM 专用代码。

## 主要功能

- 从 LT6911 HDMI 输入采集画面。
- 使用硬件编码 H.264、H.265 和 MJPEG。
- 可以识别从 640x480 到 1920x1080 的常见 HDMI 输入分辨率。
- OneKVM 提供 1080p、720p 和 480p 三档输出。1080p 最高 60 FPS；720p 可跟随 120 Hz HDMI 源。
- 可以报告 HDMI 信号状态；使用原始画面接口时还能返回内置的无信号画面。
- 可以通过官方 CVITEK SPACC 驱动加速 AES-GCM。

采集通路、CSIBDG、HDMI watcher 与 VENC reader 的约定见 [docs/video-pipeline.md](docs/video-pipeline.md)。

后端以单个动态库的形式部署：

```text
/usr/lib/onekvm/video-backends/nanokvm-mmf.so
```

该动态库由 `onekvm-device-nanokvm` 软件包安装，OneKVM 会在需要时动态加载。
本项目使用独立的标准 CMake 构建流程，不依赖 MaixCDK。

## 分辨率处理

HDMI 输入检测与 OneKVM 视频输出相互独立。目前 LT6911 输入链路可识别以下
12 种 HDMI 分辨率：

```text
1920x1080  1600x900  1440x1080  1440x900
1280x1024  1280x960  1280x800   1280x720
1152x864   1024x768  800x600    640x480
```

SG2002 的 VPSS 链路会对输入画面进行硬件缩放或裁剪，并输出为 1920x1080、
1280x720 或 640x480。1080p 最高 60 FPS；HDMI 源为 1280x720@120 时，720p
输出可以到 120 FPS。

## 驱动来源与版本

发行构建使用 Sophgo 官方源码编译内核驱动和用户态运行库，不使用 Sipeed SDK
中预编译的 MMF 组件。

所有组件统一锁定在 2026-06-30 这一批版本：

| 组件 | 官方仓库 | OneKVM 使用的版本 |
| --- | --- | --- |
| Linux 内核 | [`sophgo/linux_5.10`](https://github.com/sophgo/linux_5.10) | `sg200x-dev`，提交 `767d3c5ab10b066d2d5c7c0bd1eab8a5340e923d` |
| 视频内核驱动 | [`sophgo/osdrv`](https://github.com/sophgo/osdrv) | `sg200x-dev`，提交 `aa542c41df94f7bc656cb740f6622a5dca7dc403` |
| 视频用户态运行库 | [`sophgo/cvi_mpi`](https://github.com/sophgo/cvi_mpi) | `sg200x-dev` weekly，提交 `75c181ee6e25baca9729a4a9b415f36180b54f93` |
| LT6911 支持 | [`sophgo/SensorSupportList`](https://github.com/sophgo/SensorSupportList) | `sg200x-dev`，提交 `f064b02ba8a82746f3e87a2c5bb3bd683ff95db0` |
| 视频编解码固件 | [`sophgo/ramdisk`](https://github.com/sophgo/ramdisk) | 提交 `1ec8fcb63a358c17c369bac38eb42dc16f30a3bb` |

OSDRV 提供 SG2002 视频硬件所需的内核模块，包括
`soph_vcodec.ko`、`soph_jpeg.ko`、`soph_vi.ko` 和 `soph_vpss.ko`。

OneKVM 只在官方源码上增加少量 NanoKVM 兼容和问题修复补丁，视频驱动本身仍然
来自 Sophgo 官方 OSDRV。

这些版本构成一套配套的驱动组合。混用其他版本的内核、OSDRV 或 `cvi_mpi`
可能导致视频链路无法启动。

## 主机侧测试

基础测试不需要 SG2002 SDK，也不需要 NanoKVM 设备：

```sh
cmake -S . -B build/host \
  -DONEKVM_BUILD_MMF_BACKEND=OFF \
  -DBUILD_TESTING=ON \
  -DCMAKE_BUILD_TYPE=Release
cmake --build build/host --parallel
ctest --test-dir build/host --output-on-failure
```

这些测试会检查分辨率切换、VI 帧率解析和无信号画面生成。

## 为 NanoKVM 交叉构建

需要 SG2002 RISC-V 交叉工具链，以及已经构建好的指定版本 `sophgo/cvi_mpi`：

```sh
cmake -S . -B build/sg2002 \
  -DCMAKE_TOOLCHAIN_FILE=/path/to/sg2002-toolchain.cmake \
  -DCMAKE_OBJCOPY=/path/to/riscv64-unknown-linux-musl-objcopy \
  -DCVI_MPI_ROOT=/path/to/built/cvi_mpi \
  -DCMAKE_BUILD_TYPE=Release \
  -DBUILD_TESTING=OFF
cmake --build build/sg2002 --parallel
DESTDIR="$PWD/stage" cmake --install build/sg2002 --prefix /usr
```

默认启用厂商 SG2002 tune flags。如果工具链文件已经指定目标 ISA（例如
OpenEmbedded 工具链），应增加 `-DONEKVM_USE_VENDOR_TUNE_FLAGS=OFF`。安装过程
会同时安装后端动态库及 OneKVM device/system-plugin 描述文件。

生成可复现的发行产物时，建议使用
`onekvm-distro/scripts/oe-nanokvm-mmf-artifact.sh`。该脚本会检出锁定的官方
驱动版本、构建 `cvi_mpi`，再使用配套工具链编译本项目。

## 设备端验证

- `tests/video-backend-abi-smoke.cpp`：加载安装后的动态库并采集、编码一帧。
- `tests/video-backend-benchmark.cpp`：测试采集帧率、编码帧率、码率和编码耗时。
- `tools/lt6911-resolution-probe.c`：排查 HDMI 分辨率识别问题。

## OneKVM ABI

动态库只对外导出两个入口：

- `onekvm_video_backend_query`：视频采集和编码。
- `onekvm_crypto_backend_query`：可选的 AES-GCM 加速。

接口定义分别位于 `include/onekvm/video_backend_v1.h` 和
`include/onekvm/crypto_backend_v1.h`。两套接口都有明确版本，加载到不兼容的
动态库时，OneKVM 会在加载阶段拒绝该后端。其他 MMF 和厂商函数均保持为动态库
内部实现。

## 视频处理链路

正常使用 H.264/H.265 时，采集硬件会把画面直接交给编码器，完整的 1080p
原始画面不会经过 OneKVM Core。MJPEG 同样会把采集画面直接交给硬件 JPEG
编码器。

编码完成后，只把体积小很多的压缩结果复制到一块可重复使用的输出缓冲区，再
交给 Core。如果其他功能同时需要原始画面，后端才会切换到可能复制一帧原始
画面的通用路径。这只是内部回退方式，不是另一套对外接口。

AES-GCM 加速通过 OneKVM 的轻量适配层调用官方 `cvitek_spacc` 内核驱动。目前
H.265 和硬件加密同时运行可能锁死 SG2002，因此 H.265 会由 OneKVM Core 自动
改用普通的软件 AES-GCM，不会强行启用硬件加速。

## 无信号画面资源

源图片位于 `assets/no-signal/`。修改图片后，使用下面的命令重新生成 NV21 数据：

```sh
go run ./tools/pack_no_signal.go src/no_signal_frames.inc
```

## 许可证

本项目使用 GNU General Public License v3.0，完整条款见 [LICENSE](LICENSE)。
