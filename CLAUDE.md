# onekvm-nanokvm-mmf

给代理的短约束。架构说明在 [`docs/`](docs/README.md)，本机流水账和待办在 [`local-docs/`](local-docs/README.md)（gitignore，不进 git）。

## 文档

- 架构：[docs/video-pipeline.md](docs/video-pipeline.md)
- 功能与构建：[README.md](README.md)
- 流水账 / 待办：`local-docs/`（不要写回本文件）

## 必须遵守

- 只部署应用 IPK。不要改 U-Boot、内核、分区。`reboot` / RAUC 要先授权。
- 默认 H.264 走 `encoder_read_packet`，不要走 `source_read`。
- 活 VENC 的 `SetChnAttr` / `RequestIDR` / `close_encoder` 只在 reader 线程、两次 `GetStream` 之间做。
- CSIBDG 精确匹配。画面宽用真实尺寸；64 对齐只用于 VB stride。
- 热路径不要读 `/proc/cvitek/vi`、`vi_dbg`，也不要对活 1080 探 LT6911 I2C。HDMI 重建归 watcher。
- 不要把无信号 IDR 插进活着的 P 帧。
- 不要整树提交 `onekvm-distro`。不要在未同意时 `git push`。
