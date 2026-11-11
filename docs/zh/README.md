# 文档

[English](../en/README.md) | 简体中文

给代理和改视频的人看。本文档树只保留持续维护的接口和架构约束。
流水账、本机排查和试验机记录写入 `../../local-docs/`（gitignore），不纳入本索引。

当前默认内核是 **Linux 6.18**（`onekvm-nanokvm` 的 `PREFERRED_VERSION_linux-sophgo = "6.18%"`）。

| 文档 | 用途 |
|------|------|
| [video-pipeline.md](video-pipeline.md) | Cube HDMI 采集、绑定 H.264、释放、CSIBDG、占位、截图。改视频先读。 |
| [abi.md](abi.md) | 动态库导出的视频/加密 ABI、特性位、租约与截图约定 |
| [v4l2-decoder.md](v4l2-decoder.md) | 实验性 V4L2 解码接口、事件、缓冲和硬件资源边界 |
| [v4l2-encoder.md](v4l2-encoder.md) | 实验性 V4L2 编码接口（H.264/H.265/JPEG）、缓冲、控制与 107 实测 |
| [v4l2-capture.md](v4l2-capture.md) | 实验性 V4L2 采集接口（VI UYVY）、与 cvi_mpi 互斥、缓冲与 107 实测 |
| [v4l2-scaler.md](v4l2-scaler.md) | 实验性 V4L2 缩放与格式转换接口（VPSS）、色彩、与 cvi_mpi 并存与 107 实测 |
| [no-signal.md](no-signal.md) | 无信号 NV21 资源 |
| [dynamic-ion.md](dynamic-ion.md) | 视频／NPU 动态多媒体内存设计草案、VI DisableChn 恢复分析 |
| [overview.md](overview.md) | 功能、分辨率预算、6.18 内核、OE 构建与主机测试 |
| [../../AGENTS.md](../../AGENTS.md) | 给代理的硬约束（`CLAUDE.md` 软链到此） |

根目录 [README.zh-CN.md](../../README.zh-CN.md) 指向本文档树。

设备侧 MMF runtime、backend、IPK 必须用 `onekvm-distro` 编。禁止树外交叉编译。
