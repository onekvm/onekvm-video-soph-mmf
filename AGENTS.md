# onekvm-nanokvm-mmf

改视频先读 [docs/video-pipeline.md](docs/video-pipeline.md)。流水账/待办写 `local-docs/`（gitignore）。

设备侧 MMF runtime、backend、IPK 一律用仓库 `onekvm-distro` 的 OpenEmbedded / kas 工具链（`make nanokvm-mmf` 或 `kas.sh shell kas/nanokvm-sd.yml` 再 bitbake），不要在树外另起交叉编译。

- 只部署应用 IPK。勿改 U-Boot/内核/分区。`reboot`/RAUC 先授权。
- 勿整树提交 `onekvm-distro`。未经同意勿 `git push`。
