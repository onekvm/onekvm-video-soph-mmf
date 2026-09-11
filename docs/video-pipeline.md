# Cube HDMI 采集通路

NanoKVM Cube（SG2002）默认视频是 **LT6911 HDMI → CSI → VI → VPSS → VENC**。
Core 走绑定 H.264/H.265，不读原始帧。改采集、绑定、释放或 HDMI 探测前先读本文。
验收数据和试验过程记在工作区 `docs/`，不要把流水账写回这里。

## 拓扑

```text
HDMI 源
  → LT6911 (I2C)  → MIPI CSI
  → VI            → VPSS phy chn 1 (sc_v1)
  → VENC H.264    → reader 线程队列
  → encoder_read_packet → Core videoLoop → WebRTC
```

| 角色 | 职责 |
|------|------|
| HDMI watcher | 在 source 内跟随分辨率和热插拔。Core `videoLoop` 空闲时不会替 MMF 探 HDMI。 |
| 绑定编码器 | `BindVideoSource` + `ReadEncodedVideo`。活路上不要调 `source_read`。 |
| VENC reader | 唯一的 `GetStream` 所有者。参数集、IDR、改 FPS/码率都在这个线程、两次取流之间做。 |
| 受管截图 | `source_snapshot` 复用已有 VPSS，另开 JPEG。扩展不得再开第二个 MMF source。 |

1:1 输出也走 **VI→VPSS→VENC**。VPSS 是 online 出口，并做 UYVY→NV21。
`SYS_Bind(VI,VENC)` 可以 ioctl 成功，但解开 VI→VPSS 后 Preraw `IntCnt` 会停在 0，
`EncodedFrame=0`。双绑 VI→VPSS+VENC 会在几块 UYVY 池耗尽后变成 `VENC waitq is full`。

## 绑定路径

- `StartRecvFrame` 必须在 SYS bind 之后。否则 `enable_bind_mode` 仍为假，bind kthread 不会起来。
- 绑定 VPSS→VENC 时 `bIsoSendFrmEn` 必须关掉。绑定路径从不 `SendFrame`；打开隔离后编码计数涨、`GetStream` 队列为空。
- 首 AU 到达前不要因 VI `FrameRate=0` 走 `maybe_rebuild` 或打 LT6911 `80ee`。
- HDMI watcher 的初始信号可能晚于 WebRTC 建链。第一个 AU 到达前保留 1.5 秒 live grace，不能因 `cached_signal` 未就绪就 unbind 后直送静帧。
- Cube WAVE4 进程里只有一个 H.264/H.265 worker。绑定后不要 `DestroyChn` / `StopRecvFrame` 切换 H.264↔H.265。要换编码走设备视频设置的 `ResetVideo`（先 unbind）。
- RustDesk/RTSP 共用绑定主路；info 报实际 codec。

## 热路径

活流两帧之间队列经常为空。热路径只做：等 reader，再 `take_ready`。

不要在活包路径上：

- 读 `/proc/cvitek/vi`（整份 dump 会把 60fps 打成 30–50）
- 读 `/proc/cvitek/vi_dbg`（会 `msleep` 并卡住 CSI）
- 对活着的 1080 或更高探 LT6911 I2C（`80ee` 会打乱前端）
- `SendFrame` / `submit_h26x_frame`（再拷一整帧 NV21）
- `augment_keyframe`（IDR 已带齐 SPS/PPS 时）

像素是 VI VB → VPSS VB → VENC，硬件绑定，没有用户态 NV21 `memcpy`。
`GetStream` 必须在 `ReleaseStream` 前把压缩 AU 拷出驱动缓冲，这是唯一必要的码流拷贝。
Core 的 `take_ready` 仍会把压缩 AU 拷到 staging。

Core / `wait_ready_h26x_packet` 不要用 `std::condition_variable::wait_for`（musl/riscv64 会立刻返回），也不要 `FUTEX_WAIT_PRIVATE`（会丢唤醒，16 槽 reader 打满后变成 VENC 60 / Core 1 FPS）。现行等待是 deadline 内 1 ms `sleep_for` 轮询，保留 40 ms 超时和 stop 检查。`notify` 仍可 `FUTEX_WAKE`，但取包路径没有对应的 wait。

## VENC reader

独立线程 `GetStream`。用户态有界队列最多 16 个 AU，只吸收同步 CryptoDMA/发送的短暂调度停顿。
队满时不能等消费者，否则约 50 ms 的停顿就会把背压传回 VPSS 并触发 `VENC waitq is full`。
reader 必须立刻丢掉陈旧链、在两次 `GetStream` 之间请求一次新 IDR，并继续排空硬件流。

`SetChnAttr`（含改 FPS）和 `RequestIDR` 只能在 reader 线程、两次 `GetStream` 之间做。
HTTP 或其他线程不能对活通道直接调这些 ioctl，否则会和 `GetStream` 抢 `EnterVcodecLock`。
VPSS/VENC 延迟 proc 只在等下一包或队满时读，不要挡 `GetStream`。reader 最多 2 Hz 读
`vpss`/`venc` proc，不要读 `vi`/`vi_dbg`。status/SSE 只读缓存。

CVITEK 会把 H.264 PPS（H.265 还包括 VPS/SPS/PPS）拆成 IDR 前的独立 AU。
等 IDR 时仍须缓存这些参数集，并在关键 AU 缺项时按 VPS/SPS/PPS 顺序补齐。
丢掉所有非 IDR 会让浏览器收到 RTP 但无法初始化解码器。

只改目标 FPS、码率或 QP 时不要拆 VI/VENC 通道：reader 在两次 `GetStream` 之间改
`u32MaxBitRate` / `SetRcParam`，并补一次 IDR。不要 `close_encoder`。

## CSIBDG 与画面宽

CSI 桥是 **精确匹配**：宽度大于设定（GT）或小于设定（LS）都会把前端打挂。

- `u32PicWidth` / VENC 宽用真实 HDMI 或目标尺寸（800 就是 800）。
- 64 对齐只用于 VB stride，不要拿对齐后的值去配 CSIBDG。
- `video.resolution=0`（自动）时，VI/VENC 跟当前 **支持的** HDMI 输入。480 对应 **640×480**，不是 854×480。
- 目标分辨率只改 VPSS/VENC。VI 接收端：HDMI 变大立刻放大；HDMI 变小要等 CSI 有效尺寸跟上。HDMI 时序空白时不要缩小。
- VI 已经停帧（`FrameRate=0`）时例外：HDMI I2C 连续给出另一个支持的模式，就跟过去重建。假 1440 把 CSIBDG 配大之后，源已经回到 1080、CSI 仍是 `0x0`，再等 CSI 会对死。
- 同一几何也会卡住：VI 停帧、CSI `0x0`、I2C 仍报当前 1080 时 `should_rebuild_vi_receiver` 为假。连续 3 次同样读数后 `reopen_source` 重新 arm LT6911（含 PCIe HPD 脉冲），不要干等 CSI。占位路径必须把 `cached_signal` 置 0，否则 1080p watcher 把占位 VENC 当成活流、不再探测。
- 不要只信 HDMI I2C，也不要只信 CSI 当前宽。只信 HDMI 会在 MIPI 仍是 1920 时把 VI 配成 800；只信 CSI 会在 800→1080 时把接收端留在 800。
- CSI 已是合法模式时，忽略 I2C 垃圾 OOR 读数。

HDMI 输入不是固定模式白名单：接受 `320×200` 到 `2880×1620` 范围内、不超过 5 MP 的偶数尺寸，并按 **5 MP @ 30 FPS**（150M pixel/s）吞吐预算判定。4K 时序仍会被识别，但无论帧率均报告 `out_of_range`，不得用于重建 VI。

## HDMI 探测与 LT6911

HDMI 重建、I2C、`/proc/cvitek/vi` 归 **watcher**。

- 活 1080 或更高、且媒体已确认 VI 在跑时，watcher 必须完全跳过 VI proc 和 LT6911。即使 1 Hz VI dump 最终也会拖死 CSI。
- 无信号/未知状态时 1 s 探一次热插拔。
- 只有接收器低于 1080 且有信号时才回到 100 ms，以便及时跟 800→1080。
- 绑定编码器实际停帧后再进入恢复探测。

开机 `open_source` 分两段写 LT6911，都在 **VI init 之前**。Core 重启不会重跑 `prepare-hdmi`，所以这是 MMF 的职责。不要合成「先 D283 再 805a」的单次 80ee 会话。

1. `lt6911_kick_hdmi()`：开 `80ee`，写 `D283=0x11` 启动测量。只在打开 source 时做一次，不要从活 watcher 重复。
2. `mmf::initialize()` 回收上一代 MMF owner。旧 VI 尚未清理时重编程 CSI 会在活 HDMI 上锁死 vendor 前端。
3. `lt6911_start_csi()`：与 `prepare-hdmi` 相同，**先** `0x805a=0x80`、`0x8010=0x00`，等 100 ms，**再** `D283=0x11`，再等 50 ms，最后关 `80ee`。然后才 `start_capture_pipeline()` / `StartViChn`。

不要在 `StartViChn` 之后打 `0x805a=0x88`（CSIBDG 精确匹配，TX 掉到 0 会把 IntCnt 打成 0）。`reboot -f` 只复位 SoC，不复位 LT6911。

## 无信号占位

绑定路径不能在 HDMI 丢失后把同一个 VENC 通道改成 `SendFrame` 占位：`VPSS_UnBind` 不会清掉厂商驱动的 `currBindMode`，随后直送帧会和 `venc-handler` 在全局 VPU 锁上互锁，并耗尽 VPSS VB pool。

无信号期间保持 VPSS→VENC 绑定，只解开 **VI→VPSS**，把 NV21 占位转成 UYVY 后 `CVI_VPSS_SendFrame` 送到 group 0。WAVE4 按平常绑定路径出 IDR/P。输入恢复后重新 `VI_Bind_VPSS`。前端控制台不要叠 Vue 占位图。

`StartRecvFrame` 发生在 1.5 s live grace 里。无 HDMI 时 WAVE4 随后把静帧编成 P/skip，reader 若仍在等「自然第一帧 IDR」会把 `RequestIDR` 合并掉，WebRTC 拿不到可解码的 AU。进入占位后必须 `h26x_reader_force_idr`（清队含残留关键帧，并在 reader 线程发 ioctl），且在匹配当前 VENC 宽高的 IDR 到达前不要把 AU 交给 Core。H.264 再按 SPS 尺寸丢掉残留 CSI 包。

超过几何范围或 150M pixel/s 吞吐预算的 HDMI 模式仍在 status 中提供 `hdmi_error=out_of_range` 和实测 `input_width/height`，UI 显示「不支持的分辨率」；绑定视频同样走 VPSS 上游静帧。

VI `FrameRate` 列开机约 1 秒是 0，不能单靠这一列判无信号。判定输入消失仍要等 VENC 最近一包超过存活窗口（当前 1.5 s），避免短暂的 reader/IDR 间隙触发重建。

## 空闲与释放

无消费者时 Core `videoLoop` 停在 `waitForConsumer`。空闲用 `CVI_VPSS_DisableChn` 停掉 scaler（`CVI_VIP_SCL`），保留 VPSS→VENC、WAVE4 worker 和唯一的用户态 VENC reader；reader 进入 discard 模式，以有界 fd poll 排空停 scaler 后的迟到 AU。实际解绑会让无输入的 worker 在最终 `StopRecvFrame` 时卡进厂商锁。VI 仍跑，用来判断有没有 HDMI。

重新绑定 VENC 时先清除旧的 `last_packet_ns` 和 `read_error`，再退出 discard 并强制 IDR；这样首帧沿用 `bound_since_ns` 的 1.5 秒 grace，不会把空闲前的旧时间戳误判为 VENC 停帧并切换 placeholder。Enable 失败则保持关闭并让 Core 重试，不要拆 VI。

最终关闭先 join reader，再由同一个 MMF teardown 执行。`close_h26x_encoder` 的顺序是：

1. 若仍绑定，先 `resume_vpss_channel`（停 scaler 时 `GetStream` 可能卡在厂商锁里，需要下一帧才能观察到 stop）
2. join reader
3. 第一次 `StopRecvFrame`（SYS binding 仍启用；此时只把 channel 标成 STOP，bind kthread 还在）
4. `pause_vpss_channel`
5. Unbind
6. 第二次 `StopRecvFrame`（唤醒并 join bind kthread，清掉 `currBindMode`）
7. `ResetChn` / `DestroyChn`

不要把第一次 Stop 转交 reader 线程：第二次 Stop 会卡住并污染下一个 VENC owner。
不要在 `finish_idle_h26x_drain` 里 join：DisableChn 之后仍可能有迟到 AU，唯一 reader 必须继续排空。

驱动侧 bind worker 的生命周期由 `osdrv-sg200x` 的 bind-thread 补丁处理（显式 task 引用、completion、Destroy 前 join）。
`soph_sys`、`soph_base`、`soph_vc_driver` 必须来自同一模块包，禁止单独热替换 vc 模块。
该生命周期由 MMF 和匹配驱动处理，Core/systemd 不做模块重载或清池 workaround。

## 延迟

绑定路径没有 `acquire_capture_frame`。VENC pack `u64PTS` 是编码完成时刻，不能当采集起点。

采集是 HDMI IN 到编码器前：当前 progressive 帧 +（VI 公共池 > 2 时 VPSS waitq 一场）+ VPSS `CostTime`。
VI 公共池固定 **2** 块（fill + consume）。第三块 UYVY 会多一场采集延迟。
1080p60 大约 23 ms（16.7 + CostTime ~6.7）。

不要把 progressive 改成 50%：`VI_EARLY_INTERRUPT` 只有 proc 字段，没有 ioctl，驱动从不写 `enEalyInt`；YUV HDMI 是 `input_mem`，VPSS 等整帧进 DRAM。
不要计入 VENC waitq / `HwEncTime`（那是编码），也不要计入 VPSS `u32Depth`（截图 doneq）。编码缓存 = VENC `HwEncTime`。

## 受管截图

截图走 v1 ABI 的可选 `source_snapshot`：复用已有 source/VPSS，申请受管 JPEG encoder，把 JPEG 复制到调用者缓冲区后释放。
Core 只做鉴权和成品 JPEG 转发。扩展不得创建第二个 MMF source，也不得在绑定串流路径调用 `source_read`。
显式截图尺寸必须匹配当前输出，不能为采样重设实时流分辨率。

绑定 VPSS 输出保持 `Depth=1`，供低频截图获取最新完成帧。vendor 的满队列处理会释放旧帧、保留新帧；空闲时 DisableChn 清空队列。
帧必须保持借用直到 JPEG stream 已释放；失败时先 Stop/Destroy JPEG，再归还 VPSS 帧。原本暂停的 VPSS 在截图结束后恢复暂停。
请求期限覆盖取帧和 JPEG 送帧/读流，最长 1000 ms；结果 PTS 是 backend 单调时钟观察时刻，不是曝光时间。

已确认无信号或 `out_of_range` 时立即返回 `UNSUPPORTED`，不要用占位图制造 JPEG，也不要在占位 `SendFrame` 还握着 source 锁时去 `GetChnFrame`。

当前 policy 允许一路 H.26x 与一路 JPEG，不能据此断言芯片只能一路 H.26x。
`BACKGROUND` 用途不自动抢占实时 encoder；KVM 空闲也不表示 encoder 已销毁。后台合成需要 Core 的显式所有权交接。

## 帧率、通道与 carveout

- 采集一律走 phy chn 1（`sc_v1`，最大输出宽 2880）。phy chn 0（`sc_d`，最大 1920）上 1:1 2560 会 tile，NV21 全 0。HDMI 1080→1440 同一 PID 不换通道。
- VPSS 与 VENC 使用相同目标帧率，不再人为保持 60/30 FPS floor。720p 高刷时 `input_fps`/`TarFr` 可到 120。
- Core 绑定路径的 RTP 时戳用 `1/目标FPS`，不要用墙钟间隔。
- VPSS 和 VENC 都按输入、输出两端较严格的像素率限制帧率，避免小输入放大到 1620p 时 scaler 仍跑 60 FPS。
- 显式最大输入/输出为 **2880×1620@30**（4.67 MP、约 140 Mpixel/s），使用 **64 MiB** 视频 carveout：高于 2560×1440 时 VI 公共池两块 UYVY，VPSS 私有池仍三块。
- 720p@120 需要源真出 1280×720@120（CEA VIC 47）；Cube 默认 EDID 不含该模式。
- 活着的受支持模式不要探 LT6911 I2C。

## SRTP CryptoDMA

`write_sample` 在视频线程上同步 `block_on` 加密。完成位是 `CRYPTODMA_WR_INT`，不要 `wait_event` 活 DTB 上那条从不触发的 PLIC 59。
第一次 `ETIMEDOUT` 后进程内禁用 offload，避免再卡成 1 FPS。
不要给 crypto backend 打 `CONCURRENT_H265_VIDEO`：H.265 VENC 和 CryptoDMA 同时跑会锁死 SG2002，Core 对 H.265 会话改用软件 AES-GCM。

## 相关记录

工作区实验记录（不是本仓库的规范）：

| 文档 | 内容 |
|------|------|
| `docs/2k-30.md` | 2560×1440@30、ION 64 MiB |
| `docs/3k-30.md` | 2880×1620@30 最大输出 |
| `docs/720p-high-refresh.md` | 1280×720@120 |
| `docs/cryptodma-srtp-timeout.md` | CryptoDMA 完成位与 1 FPS 回落 |
| `docs/managed-snapshot-validation.md` | 受管截图与无信号准入 |
| `docs/recall-video-validation.md` | VPSS Depth 与截图延迟 |
| `docs/linux-5.15.md` / `docs/linux-6.18.md` | 试验机内核 bring-up |
