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
- **无消费者**：Core `videoLoop` 停在 `waitForConsumer`，不会替 MMF 探 HDMI。分辨率跟随必须在 source 自己的 **HDMI watcher** 里跑。空闲时 `CVI_VPSS_DisableChn` 停掉 scaler（`CVI_VIP_SCL`）；VI 仍跑，用来判断有没有 HDMI。绑定 VENC 或 `GetChnFrame` 时再 `EnableChn`。Enable 失败则保持关闭并让 Core 重试，不要拆 VI。
- **VENC reader**：独立线程 `GetStream`。用户态有界队列最多保留 16 个 AU，只用于吸收同步 CryptoDMA/网络发送的短暂调度停顿；队满时不能等待消费者，否则 50 ms 的停顿就会把背压传回 VPSS 并触发 `VENC waitq is full`。reader 必须立即丢弃陈旧链、在两次 `GetStream` 之间请求一次新 IDR，并继续排空硬件流。`SetChnAttr`（含改 FPS）、`RequestIDR`、`close_encoder` 也只能在这个线程、两次 `GetStream` 之间做。HTTP 或 `g_mmf_mutex` 上对活通道调这些会和 `GetStream` 抢 `EnterVcodecLock`。VPSS/VENC 延迟 proc 只在等下一包或队满时读，不要挡 `GetStream`。
- **参数集**：CVITEK 会把 H.264 PPS（H.265 还包括 VPS/SPS/PPS）拆成 IDR 前的独立 AU。reader 等待 IDR 时仍须缓存这些参数集，并在关键 AU 缺项时按 VPS/SPS/PPS 顺序补齐；直接丢掉所有非 IDR 会让浏览器收到 RTP 但无法初始化解码器。
- **绑定 VPSS→VENC** 时 `bIsoSendFrmEn` 必须关掉。绑定路径从不 `SendFrame`，打开隔离后编码计数涨、`GetStream` 队列为空。
- **首次绑定等待**：HDMI watcher 的初始信号状态可能晚于 WebRTC 建链。VENC 第一个 AU 到达前沿用 1.5 秒 live grace，不能因初始 `cached_signal` 未就绪而立即 unbind 后直送静帧；此时 VENC 里仍有 VPSS 输入，会造成 `venc-handler`/`SendFrame` 锁反转并耗尽 VPSS VB pool。
- **编码格式**：Cube WAVE4 进程里只有一个 H.264/H.265 worker。绑定后不要 `DestroyChn` / `StopRecvFrame` 切换 H.264↔H.265，VPSS 会把 `VENC waitq is full` 填满。RustDesk/RTSP 共用绑定主路，info 报实际 codec。要换编码走设备视频设置的 `ResetVideo`（先 unbind）。

## CSIBDG 与画面宽

CSI 桥是 **精确匹配**：宽度大于设定（GT）或小于设定（LS）都会把前端打挂。

- `u32PicWidth` / VENC 宽用真实 HDMI 或目标尺寸（800 就是 800）。
- 64 对齐只用于 VB stride，不要拿对齐后的值去配 CSIBDG。
- `video.resolution=0`（自动）时，VI/VENC 跟当前 **支持的** HDMI 输入。480 对应 **640x480**，不是 854x480。
- 目标分辨率只改 VPSS/VENC。VI 接收端：HDMI 变大立刻放大；HDMI 变小要等 CSI 有效尺寸跟上。HDMI 时序空白时不要缩小。
- VI 已经停帧（`FrameRate=0`）时例外：HDMI I2C 连续给出另一个支持的模式，就跟过去重建。假 1440 把 CSIBDG 配大之后，源已经回到 1080、CSI 仍是 `0x0`，再等 CSI 会对死。
- 不要只信 HDMI I2C，也不要只信 CSI 当前宽。只信 HDMI 会在 MIPI 仍是 1920 时把 VI 配成 800；只信 CSI 会在 800→1080 时把接收端留在 800。

支持的 HDMI 输入见 [README](../README.md#resolution-handling)。超范围（1366x768、4K）不要按旧几何继续采，占位并报告实测尺寸。2560x1440@30 是支持的采集/编码档。CSI 已是合法模式时，忽略 I2C 垃圾 OOR 读数。

## 热路径与 HDMI 探测

活流两帧之间队列经常为空。热路径只做：等 reader 条件变量，再 `take_ready`。

不要在活包路径上：

- 读 `/proc/cvitek/vi`（整份 debug dump，会把 60fps 打成 30–50）
- 读 `/proc/cvitek/vi_dbg`（会 `msleep` 并卡住 CSI）
- 对活着的 1080 探 LT6911 I2C（`80ee` 会打乱前端）

HDMI 重建、I2C、`/proc/cvitek/vi` 归 **watcher**。活 1080 且 VI 在跑时 watcher 也不要碰 I2C。

开机 `open_source` 写一次 LT6911 `D283=0x11` 启动 HDMI 测量。`reboot -f` 只复位 SoC，不复位 LT6911；计数停在 0x0 时需要拔插 HDMI。

## 无信号占位

绑定路径不能在 HDMI 丢失后把同一个 VENC 通道改成 `SendFrame` 占位：`VPSS_UnBind` 不会清掉厂商驱动的 `currBindMode`，随后直送帧会和 `venc-handler` 在全局 VPU 锁上互锁，并耗尽 VPSS VB pool。无信号期间保持 VPSS→VENC 绑定，只解开 **VI→VPSS**，把 NV21 占位转成 UYVY 后 `CVI_VPSS_SendFrame` 送到 group 0。WAVE4 按平常绑定路径出 IDR/P。输入恢复后重新 `VI_Bind_VPSS`。前端控制台不要叠 Vue 占位图。

`StartRecvFrame` 发生在 1.5s live grace 里。无 HDMI 时 WAVE4 随后把静帧编成 P/skip，reader 若仍在等「自然第一帧 IDR」会把 `RequestIDR` 合并掉，WebRTC 拿不到可解码的 AU。进入占位后必须 `h26x_reader_force_idr`（清队含残留关键帧，并在 reader 线程发 ioctl），且在匹配当前 VENC 宽高的 IDR 到达前不要把 AU 交给 Core。H.264 再按 SPS 尺寸丢掉残留 CSI 包。

不支持的 HDMI 模式（如 1366×768、4K）仍在 status 中提供 `hdmi_error=out_of_range` 和实测 `input_width/height`，UI 显示「不支持的分辨率」；绑定视频同样走 VPSS 上游静帧。

VI `FrameRate` 列开机约 1 秒是 0，不能单靠这一列判无信号。判定输入消失仍要等 VENC 最近一包超过存活窗口（当前 1.5s），避免短暂的 reader/IDR 间隙触发重建。

## 帧率

- 1080p 输入仍按源 60 送给 VENC。720p 高刷时 `input_fps`/`TarFr` 可到 120（`venc_src_fps` = max(60, dest)）。
- 只改目标 FPS 时不要拆 VI/VENC 通道；由 reader 在 `GetStream` 前 `SetChnAttr`。
- 只改质量预算 / 码率 / QP 同样不要拆通道：reader 在两次 `GetStream` 之间改 `u32MaxBitRate` 和 `SetRcParam`，并补一次 IDR。不要 `close_encoder`。
- Core 绑定路径的 RTP 时戳用 `1/目标FPS`，不要用墙钟间隔。
- 720p@120 已在 107 上验证：VI/VPSS FrameRate 119–120、LostFrame=0，HwEncTime ≈ 4.3 ms。HDMI 源必须真出 1280x720@120（CEA VIC 47）；Cube EDID 默认不含该模式。
- 采集一律走 phy chn 1（sc_v1，最大 2880）。phy chn 0（sc_d，最大 1920）1:1 2560 会 tile，NV21 全 0。107 r50：1080p60 与 1440p30 都在 sc_v1，WebRTC 有画面；HDMI 1080→1440 同一 PID 不换通道。详见仓库 `docs/2k-30.md`。ION **64 MiB @ 0x85000000**，公共 VB UYVY×3；不要同时留两个 `vi_vpss1` 私有池，否则 WAVE4 第二块 recon 会 OOM。`kMaxViReceiver` 仍是 1920×1080。活着的受支持模式不要探 LT6911 I2C。
