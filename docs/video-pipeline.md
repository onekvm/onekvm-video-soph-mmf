# Cube HDMI 采集通路

NanoKVM Cube（SG2002）上，默认视频是 **LT6911 HDMI → CSI → VI → VPSS → VENC**，Core 走绑定 H.264，不读原始帧。

## 运行时角色

```text
HDMI 源
  → LT6911 (I2C)  → MIPI CSI
  → VI            → VPSS
  → VENC H.264    → reader 线程队列
  → encoder_read_packet → Core videoLoop → WebRTC
```

- **绑定路径**：Core 调 `BindVideoSource` + `ReadEncodedVideo`（`encoder_read_packet`）。不要在这条路上调 `source_read`。
- **延迟**：绑定路径没有 `acquire_capture_frame`。VENC pack `u64PTS` 是编码完成时刻，不能当采集起点。采集缓存 = 一场输入周期（`1/input_fps`）+ `/proc/cvitek/vpss` 的 `CostTime`；编码缓存 = `/proc/cvitek/venc` 的 `HwEncTime`。reader 最多 2Hz 读这两份 proc（不要读 `vi`/`vi_dbg`）。status/SSE 只读缓存。
- **无消费者**：Core `videoLoop` 停在 `waitForConsumer`，不会替 MMF 探 HDMI。分辨率跟随必须在 source 自己的 **HDMI watcher** 里跑。
- **VENC reader**：独立线程 `GetStream`。用户态队列只保留 1 个 AU，避免预取 4 帧。`SetChnAttr`（含改 FPS）、`RequestIDR`、`close_encoder` 只能在这个线程、两次 `GetStream` 之间做。HTTP 或 `g_mmf_mutex` 上对活通道调这些会和 `GetStream` 抢 `EnterVcodecLock`。VPSS/VENC 延迟 proc 只在等下一包或队满时读，不要挡 `GetStream`。
- **绑定 VPSS→VENC** 时 `bIsoSendFrmEn` 必须关掉。绑定路径从不 `SendFrame`，打开隔离后编码计数涨、`GetStream` 队列为空。

## CSIBDG 与画面宽

CSI 桥是 **精确匹配**：宽度大于设定（GT）或小于设定（LS）都会把前端打挂。

- `u32PicWidth` / VENC 宽用真实 HDMI 或目标尺寸（800 就是 800）。
- 64 对齐只用于 VB stride，不要拿对齐后的值去配 CSIBDG。
- `video.resolution=0`（自动）时，VI/VENC 跟当前 **支持的** HDMI 输入。480 对应 **640x480**，不是 854x480。
- 目标分辨率只改 VPSS/VENC。VI 接收端：HDMI 变大立刻放大；HDMI 变小要等 CSI 有效尺寸跟上。HDMI 时序空白时不要缩小。
- 不要只信 HDMI I2C，也不要只信 CSI 当前宽。只信 HDMI 会在 MIPI 仍是 1920 时把 VI 配成 800；只信 CSI 会在 800→1080 时把接收端留在 800。

支持的 HDMI 输入见 [README](../README.md#resolution-handling)。超范围（1366x768、1440p、4K）不要按旧几何继续采，占位并报告实测尺寸。CSI 已是合法模式时，忽略 I2C 垃圾 OOR 读数。

## 热路径与 HDMI 探测

活流两帧之间队列经常为空。热路径只做：等 reader 条件变量，再 `take_ready`。

不要在活包路径上：

- 读 `/proc/cvitek/vi`（整份 debug dump，会把 60fps 打成 30–50）
- 读 `/proc/cvitek/vi_dbg`（会 `msleep` 并卡住 CSI）
- 对活着的 1080 探 LT6911 I2C（`80ee` 会打乱前端）

HDMI 重建、I2C、`/proc/cvitek/vi` 归 **watcher**。活 1080 且 VI 在跑时 watcher 也不要碰 I2C。

开机 `open_source` 写一次 LT6911 `D283=0x11` 启动 HDMI 测量。`reboot -f` 只复位 SoC，不复位 LT6911；计数停在 0x0 时需要拔插 HDMI。

## 无信号占位

无信号图是缓存的 NV21 素材（由 PNG 打包装入 `no_signal_frames.inc`）。绑定路径在 HDMI 丢失后 **解绑 VPSS**，把这帧 `SendFrame` 进 VENC，让码率控制出 P 帧。不要每圈塞一份预编码 IDR，那会把码率打到十几 Mbps。

不支持的 HDMI 模式（如 1366×768、1440p）同样走这套占位编码，但 status 带 `hdmi_error=out_of_range` 和实测 `input_width/height`，UI 显示「不支持的分辨率」，不要只显示无信号。

不要把占位 IDR 插进还在出的 P 帧。VI `FrameRate` 列开机约 1 秒是 0，不能单靠这一列判无信号。占位图要等 VENC 最近一包超过存活窗口（当前 1.5s）。

## 帧率

- 输入侧按源 60 送给 VENC（`input_fps=60`），输出 `TarFr` 跟用户目标 FPS。
- 只改目标 FPS 时不要拆 VI/VENC 通道；由 reader 在 `GetStream` 前 `SetChnAttr`。
- Core 绑定路径的 RTP 时戳用 `1/目标FPS`，不要用墙钟间隔。
