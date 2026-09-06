改视频先读 `docs/video-pipeline.md`。

设备侧 MMF runtime、backend、IPK 必须用仓库 `onekvm-distro` 的 OpenEmbedded/kas 编：`make mmf`，或 `kas.sh shell kas/nanokvm-sd.yml` 再 bitbake。禁止树外交叉编译。

只部署应用 IPK。禁止改 U-Boot、内核、分区。`reboot`/RAUC 先问。禁止整树提交 `onekvm-distro`。未经同意禁止 `git push`。流水账写 `local-docs/`（gitignore）。
