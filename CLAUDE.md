# onekvm-nanokvm-mmf 约束

本文件只放仍然约束下次行为的规则。排查流水账和待办在本仓库 `local-docs/`（已 gitignore）。git 里的 TODO 文件不要删。

## 部署

- 只部署应用 IPK。`10.100.99.137` 不在 NFS export，用 scp + `opkg install --force-reinstall --force-depends`。
- 设备卡死时 `onekvm-server` 可能 SIGKILL 无效，只能 `reboot -f`。这只复位 SoC，不复位 LT6911；HDMI 计数 0x0 时要拔插线。

## 采集 / 编码

- 默认 H.264 走绑定路径（`encoder_read_packet`），不要走 `source_read`。
- 活 VENC 上的 `SetChnAttr` / `RequestIDR` / `close_encoder` 只能在 reader 线程、两次 `GetStream` 之间做。
- CSIBDG 精确匹配。画面宽用真实 HDMI/目标尺寸，64 对齐只用于 VB stride，不要改 `u32PicWidth`。
- `video.resolution=0` 时 VI/VENC 跟当前支持的 HDMI 输入；480 是 640x480，不是 854x480。
- 目标分辨率只改 VPSS/VENC。HDMI 变大立刻放大 VI 接收端；HDMI 变小要等 CSI 有效尺寸跟上。HDMI 空白时不要缩小。
- 没有 WebRTC 消费者时 Core `videoLoop` 停在 `waitForConsumer`。HDMI 探测必须在 MMF source 的 watcher 里跑。
- 活 1080 不要探 LT6911 I2C。活流空队列不要读 `/proc/cvitek/vi` 或握锁睡；等 reader cv。HDMI 重建归 watcher。
- 不要把无信号占位 IDR 插进活着的 P 帧。CSI 是合法模式时忽略垃圾 OOR。占位图要等 VENC 停够窗口。
- 超范围 HDMI（1366x768 / 1440p / 4K）不得按旧几何继续采，应占位并报告实测尺寸。
- 开机 `open_source` 写一次 `D283=0x11`，否则 HDMI 测量可能不起。
- 0 FPS 可能只是显示器休眠，不是进程卡死；看门狗只认 D-Bus 心跳。
