# onekvm-nanokvm-mmf

[English](README.md) | 简体中文

OneKVM 面向 NanoKVM 的视频与 SRTP 加密硬件后端。它把 SG2002 多媒体链路接到
OneKVM Backend ABI，并把厂商 MMF 实现留在独立动态库里，避免 `onekvm-server`
直接依赖 NanoKVM 专用代码。

设备侧构建必须走 `onekvm-distro` 的 OpenEmbedded/kas。禁止树外交叉编译。

## 主要功能

- 从 LT6911 HDMI 输入采集画面。
- 硬件编码 H.264、H.265 和 MJPEG。
- 接受 320×200 到 2880×1620 范围内、不超过 5 MP 的偶数 HDMI 尺寸，并按
  5 MP@30 FPS（150M pixel/s）吞吐预算判定；不支持 4K 输入。
- 输出档最高 2880×1620@30。1080p 最高 60 FPS，720p 可跟随 120 Hz HDMI 源。
- 绑定编码路径：VPSS 直接交给 VENC。Core 只读压缩 AU，不读原始 1080p 帧。
- 可以报告 HDMI 信号、sink EDID、采集/编码延迟；原始帧回退路径还能返回内置无信号画面。
- 可通过 CVITEK SPACC 字符设备做可选 AES-GCM 加速。

采集通路、CSIBDG、HDMI watcher、VENC reader、空闲释放与截图约定见
[docs/video-pipeline.md](docs/video-pipeline.md)。ABI 见
[docs/abi.md](docs/abi.md)。

后端以单个动态库部署：

```text
/usr/lib/onekvm/video-backends/nanokvm-mmf.so
```

由 `onekvm-device-nanokvm` 安装，OneKVM 在需要时动态加载。本项目使用独立
CMake，不依赖 MaixCDK。

## 分辨率处理

HDMI 输入检测与 OneKVM 视频输出相互独立。LT6911 输入不是模式白名单：
在 320×200 到 2880×1620 范围内，偶数尺寸只要不超过 5 MP，并满足 150M pixel/s
预算即可采集。4K 时序仍会被识别，但无论帧率都报告 `out_of_range`。

可选择的输出档：

```text
2880x1620  2560x1440  1920x1080  1600x900  1440x1080  1440x900
1280x1024  1280x960  1280x800   1280x720
1152x864   1024x768  800x600    640x480
```

广告给 Core 的 ABI 表是常用子集：
2880×1620@30、2560×1440@30、1920×1080@60、1280×720@60、640×480@60。

SG2002 采集一律走 VPSS phy 通道 1（`sc_v1`，最大宽 2880）。通道 0（`sc_d`）
最大宽只有 1920，2560 的 1:1 转换会 tile，NV21 全 0。自动模式会跟随可直接输出
的 HDMI 输入。显式 2880×1620 是最大的原生 16:9 输出档；高于 2560×1440 的输入
使用两块 UYVY VI 缓冲，VPSS 私有池仍为三块，可装入 NanoKVM 的 64 MiB 视频
carveout。该档受像素预算限制，最高 30 FPS。HDMI 源为 1280×720@120 时，720p
可以到 120 FPS。

## 驱动来源

用户态 MMF 库来自 Sophgo 官方源码，锁定在同一批 2026-06-30 版本。不使用
Sipeed SDK 里预编译的 MMF 组件。

| 组件 | 来源 | 锁定版本 |
| --- | --- | --- |
| 视频用户态（`cvi_mpi`） | [`sophgo/cvi_mpi`](https://github.com/sophgo/cvi_mpi) `sg200x-dev` | `75c181ee6e25baca9729a4a9b415f36180b54f93` |
| LT6911 传感器列表 | [`sophgo/SensorSupportList`](https://github.com/sophgo/SensorSupportList) `sg200x-dev` | `f064b02ba8a82746f3e87a2c5bb3bd683ff95db0` |
| 编译 `cvi_mpi` 用的 osdrv | [`sophgo/osdrv`](https://github.com/sophgo/osdrv) `sg200x-dev` | `aa542c41df94f7bc656cb740f6622a5dca7dc403` |
| 视频编解码固件 | [`sophgo/ramdisk`](https://github.com/sophgo/ramdisk) | `1ec8fcb63a358c17c369bac38eb42dc16f30a3bb` |
| 视频内核模块 | 工作区 `osdrv-sg200x`（Sophgo osdrv + NanoKVM 补丁） | 配方 `onekvm-device-nanokvm-mmf-modules` |

OSDRV 模块包括 `soph_vcodec.ko`、`soph_jpeg.ko`、`soph_vi.ko`、`soph_vpss.ko`。
bind 线程回收以及 5.15/6.18 兼容补丁在模块配方里，不在本仓库。运行中的 Linux
由 `onekvm-distro`（`linux-sophgo`）选择，本仓库不锁定内核。

不要把不同版本的 `cvi_mpi`、osdrv 或固件和此后端混用。必须一起安装的三个 IPK：

- `onekvm-device-nanokvm-mmf-runtime` — `cvi_mpi` 用户态库
- `onekvm-device-nanokvm-mmf-modules` — `soph_*` 内核模块
- `onekvm-device-nanokvm` — 本后端 `.so`

## 主机侧测试

不需要 SG2002 SDK，也不需要设备：

```sh
cmake -S . -B build/host \
  -DONEKVM_BUILD_MMF_BACKEND=OFF \
  -DBUILD_TESTING=ON \
  -DCMAKE_BUILD_TYPE=Release
cmake --build build/host --parallel
ctest --test-dir build/host --output-on-failure
```

覆盖分辨率策略、输出帧率钳位、VI 帧率解析、H.264 annex B、硬件延迟 proc
解析、EDID 板型、截图准入和无信号画面。

## 为设备构建

在 `onekvm-distro` 中，机型 `onekvm-nanokvm`：

```sh
make package mmf
```

会编 `onekvm-device-nanokvm-mmf-runtime` 和 `onekvm-device-nanokvm`。等价写法：

```sh
./scripts/kas.sh shell kas/nanokvm-sd.yml \
  -c 'bitbake -c package_write_ipk onekvm-device-nanokvm-mmf-runtime onekvm-device-nanokvm'
```

用当前工作树编、不改 recipe `SRCREV`：

```sh
ONEKVM_DEBUG_WORKTREES=onekvm-nanokvm-mmf \
  ./scripts/kas.sh shell kas/nanokvm-sd.yml \
  -c 'bitbake -c package_write_ipk onekvm-device-nanokvm'
```

详见 `onekvm-distro/docs/debug-build.md`。只部署应用 IPK，然后重启
`onekvm.service`。未经明确授权不要改 U-Boot、内核或分区。

设备端 smoke（`tests/bound-reader-stress.cpp`、
`tests/managed-snapshot-smoke.cpp`）只有同时打开 `ONEKVM_BUILD_MMF_BACKEND`
和 `BUILD_TESTING` 才会编。OE 配方把 `-DBUILD_TESTING=OFF` 写死，debug
worktree 不会改这个开关。需要这两份二进制时再覆盖 `EXTRA_OECMAKE`。

`tools/lt6911-resolution-probe.c` 不是 CMake 目标。量产 `.so` 除两个 query
外全部 hidden，探针对已安装的库 `dlsym` 不到 LT6911 符号。排查用 watcher
日志或调试构建里的 I2C。

## OneKVM ABI

动态库只导出两个入口：

- `onekvm_video_backend_query`
- `onekvm_crypto_backend_query`

接口在 `include/onekvm/video_backend_v1.h` 和
`include/onekvm/crypto_backend_v1.h`，都有明确版本。H.265 会话不广告硬件加密，
Core 改用软件 AES-GCM。CryptoDMA 第一次 `ETIMEDOUT` 后进程内禁用 offload。

## 无信号画面资源

源图片在 `assets/no-signal/`。改图后重新生成 NV21 数据：

```sh
go run ./tools/pack_no_signal.go src/no_signal_frames.inc
```

运行时不解析 PNG。绑定无信号静帧以 NV21 送进 VPSS
（`render_no_signal_nv21` → `submit_vpss_nv21`），WAVE4 仍走平常绑定路径编码。
`src/no_signal_h264.inc` 只给主机测试用。

## 许可证

GNU General Public License v3.0，完整条款见 [LICENSE](LICENSE)。
