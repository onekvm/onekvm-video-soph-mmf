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
- **无消费者**：Core `videoLoop` 停在 `waitForConsumer`，不会替 MMF 探 HDMI。分辨率跟随必须在 source 自己的 **HDMI watcher** 里跑。空闲快照用 `CVI_VPSS_DisableChn` 停掉 scaler（`CVI_VIP_SCL`），保留 VPSS→VENC、WAVE4 worker 和唯一的用户态 VENC reader；reader 进入 discard 模式，以有界 fd poll 排空停 scaler 后的迟到 AU。实际解绑会让无输入的 worker 在最终 `StopRecvFrame` 时卡进厂商锁。VI 仍跑，用来判断有没有 HDMI。重新绑定 VENC 时先清除旧的 `last_packet_ns` 和 `read_error`，再退出 discard 并强制 IDR；这样首帧沿用 `bound_since_ns` 的 1.5 秒 grace，不会把空闲前的旧时间戳误判为 VENC 停帧并切换 placeholder。最终释放依次恢复 scaler、第一次 Stop、暂停 scaler、Unbind、第二次 Stop 和 Destroy；两次 Stop 对应驱动 bind 状态机的两个阶段。Enable 失败则保持关闭并让 Core 重试，不要拆 VI。
- **热重启释放**：107 的 r55（`f9ff576`）在活跃 1080p60 WebRTC 下会停止超过 60 秒并触发 watchdog；仅修正 Stop → Unbind 后，r56/r57 虽能约 5 秒退出，新进程仍是 VPSS 60 FPS、VENC `EncodedFrame=0`。驱动的第一次 `StopRecvFrame` 发生在 SYS binding 仍启用时，只会标记 channel STOP，不会停止 bind kthread；r58（`5bf4ea9`）先保留 producer 完成第一次 Stop，暂停 VPSS 后 Unbind，再用第二次 Stop 唤醒并 join bind kthread。不要把第一次 Stop 转交 reader 线程：该实验会让第二次 Stop 卡住并污染下一个 VENC owner。后续实测发现 bind worker 自行退出后仍发布已释放的 `task_struct`，第二次 Stop 会在 `kthread_stop` Oops。OE 模块包 r24 用显式 task 引用、completion 和互斥锁串行回收，并在 Destroy 释放 jobs/信号量/通道前 join；共享 context 布局要求 `soph_sys`、`soph_base`、`soph_vc_driver` 使用同一包并在重启后一起生效，禁止单独热替换 vc 模块。r24 首轮活流重启约 3 秒，PID 525→818、boot ID 不变，重连恢复清晰 1080p60 和真实 JPEG。bind/teardown 仍观察到瞬时 `VENC waitq is full`，当前样本没有持续停滞或 Oops；第二轮及长时间稳定性仍须继续验证。该生命周期由 MMF 和匹配驱动处理，Core/systemd 不做模块重载或清池 workaround。
- **热路径拷贝**：绑定路径像素是 VI VB → VPSS VB → VENC，硬件绑定，没有用户态 NV21 `memcpy`（那条 186 MB/s 的拷贝已否）。`GetStream` 必须在 `ReleaseStream` 前把压缩 AU 拷出驱动缓冲，这是唯一必要的码流拷贝。reader 把该缓冲 `assign` 进队列；Core 的 memcpy `take_ready` 仍拷一次压缩 AU 到 1 MB staging（`take_ready_h26x_into` 的 swap 路径还没接到 bound drain）。IDR 已带齐 SPS/PPS 时不 `augment_keyframe`。禁止在绑定活路上 `SendFrame`/`submit_h26x_frame`（那会再拷一整帧 NV21）。
- **SRTP CryptoDMA**：`write_sample` 在视频线程上同步 `block_on` 加密。完成位是 `CRYPTODMA_WR_INT`，不要 `wait_event` 活 DTB 上那条从不触发的 PLIC 59。第一次 `ETIMEDOUT` 后进程内禁用 offload，避免再卡成 1 FPS。
- **VENC reader**：独立线程 `GetStream`。用户态有界队列最多保留 16 个 AU，只用于吸收同步 CryptoDMA/网络发送的短暂调度停顿；队满时不能等待消费者，否则 50 ms 的停顿就会把背压传回 VPSS 并触发 `VENC waitq is full`。reader 必须立即丢弃陈旧链、在两次 `GetStream` 之间请求一次新 IDR，并继续排空硬件流。`SetChnAttr`（含改 FPS）和 `RequestIDR` 只能在 reader 线程、两次 `GetStream` 之间做；最终关闭则先 join reader，再由同一个 MMF teardown 顺序执行 Stop/Unbind/Stop/Destroy。HTTP 或其他线程不能对活通道直接调这些 ioctl，否则会和 `GetStream` 抢 `EnterVcodecLock`。VPSS/VENC 延迟 proc 只在等下一包或队满时读，不要挡 `GetStream`。Core 取队列时不要用 `std::condition_variable::wait_for`：目标机 musl/riscv64 会提前返回并让视频线程在帧间空转；使用 generation counter + private futex，在 producer 更新 generation 后唤醒，并保留 40 ms 超时和 stop 检查。
- **参数集**：CVITEK 会把 H.264 PPS（H.265 还包括 VPS/SPS/PPS）拆成 IDR 前的独立 AU。reader 等待 IDR 时仍须缓存这些参数集，并在关键 AU 缺项时按 VPS/SPS/PPS 顺序补齐；直接丢掉所有非 IDR 会让浏览器收到 RTP 但无法初始化解码器。
- **绑定 VPSS→VENC** 时 `bIsoSendFrmEn` 必须关掉。绑定路径从不 `SendFrame`，打开隔离后编码计数涨、`GetStream` 队列为空。
- **首次绑定等待**：HDMI watcher 的初始信号状态可能晚于 WebRTC 建链。VENC 第一个 AU 到达前沿用 1.5 秒 live grace，不能因初始 `cached_signal` 未就绪而立即 unbind 后直送静帧；此时 VENC 里仍有 VPSS 输入，会造成 `venc-handler`/`SendFrame` 锁反转并耗尽 VPSS VB pool。
- **编码格式**：Cube WAVE4 进程里只有一个 H.264/H.265 worker。绑定后不要 `DestroyChn` / `StopRecvFrame` 切换 H.264↔H.265，VPSS 会把 `VENC waitq is full` 填满。RustDesk/RTSP 共用绑定主路，info 报实际 codec。要换编码走设备视频设置的 `ResetVideo`（先 unbind）。

## 受管截图与低频采样

截图通过 v1 ABI 的可选 `source_snapshot` 完成：复用已有 source/VPSS，申请受管 JPEG encoder，将 JPEG 复制到调用者缓冲区后释放资源。Core 只做鉴权和成品 JPEG 转发；扩展不得创建第二个 MMF source，也不得在绑定串流路径调用 `source_read`。显式截图尺寸必须匹配当前输出，不能为采样重设实时流分辨率。

绑定 VPSS 输出保持 `Depth=1`，供低频截图获取最新完成帧。vendor 的满队列处理会释放旧帧，保留新帧；空闲时 DisableChn 清空队列。帧必须保持借用直到 JPEG stream 已释放，失败时先 Stop/Destroy JPEG，再归还 VPSS 帧。原本暂停的 VPSS 在截图结束后恢复暂停。请求期限覆盖取帧和 JPEG 送帧/读流，最长 1000 ms；结果 PTS 是 backend 单调时钟观察时刻，不是曝光时间。Depth 变更的实机延迟和帧率结果记在工作区 `docs/recall-video-validation.md`。

encoder 分配通过 `encoder_resources` / `encoder_allocate` / `encoder_allocation` 管理，handle 销毁即释放租约。当前 policy 允许一路 H.26x 与一路 JPEG，不能据此断言芯片只能一路 H.26x；后台多路组合仍须独立实测。`BACKGROUND` 用途不自动抢占实时 encoder，KVM 空闲也不表示 encoder 已销毁；后台合成需要 Core 的显式所有权交接。

## CSIBDG 与画面宽

CSI 桥是 **精确匹配**：宽度大于设定（GT）或小于设定（LS）都会把前端打挂。

- `u32PicWidth` / VENC 宽用真实 HDMI 或目标尺寸（800 就是 800）。
- 64 对齐只用于 VB stride，不要拿对齐后的值去配 CSIBDG。
- `video.resolution=0`（自动）时，VI/VENC 跟当前 **支持的** HDMI 输入。480 对应 **640x480**，不是 854x480。
- 目标分辨率只改 VPSS/VENC。VI 接收端：HDMI 变大立刻放大；HDMI 变小要等 CSI 有效尺寸跟上。HDMI 时序空白时不要缩小。
- VI 已经停帧（`FrameRate=0`）时例外：HDMI I2C 连续给出另一个支持的模式，就跟过去重建。假 1440 把 CSIBDG 配大之后，源已经回到 1080、CSI 仍是 `0x0`，再等 CSI 会对死。
- 不要只信 HDMI I2C，也不要只信 CSI 当前宽。只信 HDMI 会在 MIPI 仍是 1920 时把 VI 配成 800；只信 CSI 会在 800→1080 时把接收端留在 800。

HDMI 输入不是固定模式白名单：接受 `320×200` 到 `2880×1620` 范围内、不超过 5 MP 的偶数尺寸，并按 **5 MP @ 30 FPS**（150M pixel/s）吞吐预算判定。4K 时序仍会被识别，但无论帧率均报告 `out_of_range`，不得用于重建 VI。CSI 已是合法模式时，忽略 I2C 垃圾 OOR 读数。

## 热路径与 HDMI 探测

活流两帧之间队列经常为空。热路径只做：等 reader 条件变量，再 `take_ready`。

不要在活包路径上：

- 读 `/proc/cvitek/vi`（整份 debug dump，会把 60fps 打成 30–50）
- 读 `/proc/cvitek/vi_dbg`（会 `msleep` 并卡住 CSI）
- 对活着的 1080 探 LT6911 I2C（`80ee` 会打乱前端）

HDMI 重建、I2C、`/proc/cvitek/vi` 归 **watcher**。活 1080 或更高且媒体已确认 VI 在跑时，watcher 必须完全跳过 VI proc 和 LT6911；即使 1 Hz VI dump 最终也会拖死 CSI。无信号/未知状态时 1 s 探一次热插拔；只有接收器低于 1080 且有信号时才回到 100 ms，以便及时跟 800→1080。绑定编码器实际停帧后再进入恢复探测。

开机 `open_source` 在 **VI init 之前**写 LT6911：`D283=0x11` 启动测量，再按 `prepare-hdmi` 写 `0x805a=0x80`、`0x8010=0x00`，等 100ms 后再关 `80ee`。Core 重启不会重跑 `prepare-hdmi`，所以这是 MMF 的职责。顺序必须是 `mmf::initialize()` 先回收上一代 MMF owner，之后 arm LT6911，最后启动 VI；旧 VI 尚未清理时重编程 CSI 会在活 HDMI 上锁死 vendor 前端。不要在 `StartViChn` 之后打 `0x805a=0x88`（CSIBDG 精确匹配，TX 掉到 0 会把 IntCnt 打成 0）。`reboot -f` 只复位 SoC，不复位 LT6911。

## 无信号占位

绑定路径不能在 HDMI 丢失后把同一个 VENC 通道改成 `SendFrame` 占位：`VPSS_UnBind` 不会清掉厂商驱动的 `currBindMode`，随后直送帧会和 `venc-handler` 在全局 VPU 锁上互锁，并耗尽 VPSS VB pool。无信号期间保持 VPSS→VENC 绑定，只解开 **VI→VPSS**，把 NV21 占位转成 UYVY 后 `CVI_VPSS_SendFrame` 送到 group 0。WAVE4 按平常绑定路径出 IDR/P。输入恢复后重新 `VI_Bind_VPSS`。前端控制台不要叠 Vue 占位图。

`StartRecvFrame` 发生在 1.5s live grace 里。无 HDMI 时 WAVE4 随后把静帧编成 P/skip，reader 若仍在等「自然第一帧 IDR」会把 `RequestIDR` 合并掉，WebRTC 拿不到可解码的 AU。进入占位后必须 `h26x_reader_force_idr`（清队含残留关键帧，并在 reader 线程发 ioctl），且在匹配当前 VENC 宽高的 IDR 到达前不要把 AU 交给 Core。H.264 再按 SPS 尺寸丢掉残留 CSI 包。

超过几何范围或 150M pixel/s 吞吐预算的 HDMI 模式仍在 status 中提供 `hdmi_error=out_of_range` 和实测 `input_width/height`，UI 显示「不支持的分辨率」；绑定视频同样走 VPSS 上游静帧。

VI `FrameRate` 列开机约 1 秒是 0，不能单靠这一列判无信号。判定输入消失仍要等 VENC 最近一包超过存活窗口（当前 1.5s），避免短暂的 reader/IDR 间隙触发重建。

## 帧率

- VPSS 与 VENC 使用相同目标帧率，不再人为保持 60/30 FPS floor，避免因上下游 cadence 不一致而额外丢帧。720p 高刷时 `input_fps`/`TarFr` 可到 120。
- 只改目标 FPS 时不要拆 VI/VENC 通道；由 reader 在 `GetStream` 前 `SetChnAttr`。
- 只改质量预算 / 码率 / QP 同样不要拆通道：reader 在两次 `GetStream` 之间改 `u32MaxBitRate` 和 `SetRcParam`，并补一次 IDR。不要 `close_encoder`。
- Core 绑定路径的 RTP 时戳用 `1/目标FPS`，不要用墙钟间隔。
- 720p@120 已在 107 上验证：VI/VPSS FrameRate 119–120、LostFrame=0，HwEncTime ≈ 4.3 ms。HDMI 源必须真出 1280x720@120（CEA VIC 47）；Cube EDID 默认不含该模式。
- 采集一律走 phy chn 1（sc_v1，最大输出宽 2880）。phy chn 0（sc_d，最大 1920）1:1 2560 会 tile，NV21 全 0。107 r50：1080p60 与 1440p30 都在 sc_v1，WebRTC 有画面；HDMI 1080→1440 同一 PID 不换通道。详见仓库 `docs/2k-30.md`。显式最大输入/输出为 **2880×1620@30**（4.67 MP、139.97 Mpixel/s），使用 **64 MiB** 视频 carveout：高于 2560×1440 时 VI 公共池使用两块 UYVY 缓冲，VPSS 私有池仍为三块。VPSS 和 VENC 都按输入、输出两端较严格的像素率限制帧率，避免小输入放大到 1620p 时 scaler 仍跑 60 FPS。验收时同时核对 ION 分配、VENC/VPSS FPS、LostFrame 和浏览器画面。`kMaxViReceiver` 仍是 1920×1080。活着的受支持模式不要探 LT6911 I2C。
