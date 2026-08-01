# onekvm-device-nanokvm-mmf

OneKVM 维护的 NanoKVM/SG2002 多媒体运行库，提供 HDMI 视频采集以及
H.264、H.265、MJPEG 硬件编码。

本仓库是独立的 MaixCDK `kvm_mmf` component，不再通过补丁修改
Sipeed NanoKVM 仓库。构建时将本仓库作为 `components/kvm_mmf` 使用，
Sophgo middleware、LT6911 sensor glue 和交叉工具链仍由 NanoKVM/MaixCDK
构建环境提供。

## ABI

公共 ABI 定义在 `include/kvm_mmf.hpp`。OneKVM runtime 升级必须保持：

- `libkvm_mmf.so` SONAME
- 已有动态导出符号
- `mmf_stream_t` 和 `mmf_venc_cfg_t` 的内存布局

## 性能设计

- VI 映射按物理地址缓存，避免逐帧 mmap/munmap。
- VI 帧可直接送入 VENC，外部缓冲区仍走兼容复制路径。
- VENC/JPEG pack 使用通道级复用缓冲，避免逐帧 malloc/free。
- 每个编码通道最多保留一帧在途，避免实时视频积压旧帧。
- vendor pack 超过公共 ABI 的 8 项时安全合并编码数据。

## 构建与验证

发行构建由 `onekvm-distro/scripts/oe-nanokvm-mmf-artifact.sh` 调用。真机
验证程序位于 `tests/nanokvm-mmf-smoke.cpp`。

## 来源与许可

最初代码来自 Sipeed NanoKVM 2.4.3，OneKVM 在其基础上维护 ABI、H.265、
资源回收和性能修复。项目按 GPL-3.0 授权，详见 `LICENSE`。
