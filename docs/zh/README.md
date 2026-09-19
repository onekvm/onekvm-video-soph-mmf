# 文档

[English](../en/README.md) | 简体中文

给代理和改视频的人看。本文档树只保留持续维护的接口和架构约束。
流水账、本机排查和试验机记录写入 `../../local-docs/`（gitignore），不纳入本索引。

当前默认内核是 **Linux 6.18**（`onekvm-nanokvm` 的 `PREFERRED_VERSION_linux-sophgo = "6.18%"`）。

| 文档 | 用途 |
|------|------|
| [video-pipeline.md](video-pipeline.md) | Cube HDMI 采集、绑定 H.264、释放、CSIBDG、占位、截图。改视频先读。 |
| [abi.md](abi.md) | 动态库导出的视频/加密 ABI、特性位、租约与截图约定 |
| [no-signal.md](no-signal.md) | 无信号 NV21 资源 |
| [overview.md](overview.md) | 功能、分辨率预算、6.18 内核、OE 构建与主机测试 |
| [../../AGENTS.md](../../AGENTS.md) | 给代理的硬约束（`CLAUDE.md` 软链到此） |

根目录 [README.zh-CN.md](../../README.zh-CN.md) 指向本文档树。

设备侧 MMF runtime、backend、IPK 必须用 `onekvm-distro` 编。禁止树外交叉编译。
