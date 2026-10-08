# Cube HDMI 采集通路

[English documentation index](../en/README.md) | 简体中文

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
- HDMI watcher 的初始信号可能晚于 WebRTC 建链。新绑定或从空闲恢复后，第一个 AU 到达前保留 5 秒首帧窗口；1.5 秒只用于已经出过 AU 的活流停顿。不能因 `cached_signal` 未就绪就 unbind 后直送静帧。
- 独占 VI→VENC 超过首帧窗口仍完全没有 AU 时，必须撤销绑定时乐观设置的 `cached_signal=1` 并启动静默失锁探测；不能无限期只请求 IDR。连续 CSI `0x0`、HDMI 仍报当前尺寸时，仅在有绑定消费者且 VI DMA 运行时累计软件重武装样本，不能把参数固定成 `false` 导致恢复门永远打不开。软件恢复重开 source 时不能用旧的 HDMI active-size 计数推断 CSI 已锁定并跳过 `lt6911_start_csi()`。完整 PCIe GPIO 复位后的重开则可以保留真正恢复的桥片锁定。
- PCIe 冷启动在 CSI 武装前探测 HDMI 2.5 s。优先用窗口内稳定/最大的合法 timing，其次用 `/run/onekvm/last-hdmi-input` 记住的活路分辨率，再才回退 640×480。绑定 640/720×480 bootstrap 时不要把 `cached_signal` 设成 live，否则 watcher 要等占位路径才会 grow 到 1080p，WebRTC 会先锁在 640 SPS。
- 活路 HDMI watcher 用静默探测：只读 0xd28b/0x85ea/0xe08c 计数，不写 `0x80ee`、不打 `D283`。PCIe 640/720×480 bootstrap 在还没有消费者时也静默探测。完整快照用于 CSI/VI 启动前，以及已进入占位、VI 不再接收真实帧的断信号恢复期；真实 AU 一旦恢复就停止完整快照。
- 冷启动若已经读到桥片锁定的合法时序，无消费者时不要仅凭静默计数暂时为零就把接收端盲目升到 1080p；这会破坏已锁定的 720×480 输入。只有 fallback bootstrap 才需要空闲 grow 探测，或者等绑定消费者后以真实 AU 判断。
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
reader 先检查所有 pack 的长度、偏移和容量，再把 AU 直接拷进可复用 vector；
队列、`take_ready_h26x_into` 与 `encoder_take_packet` 逐级移动所有权，不再经过独立 scratch。
Core 的 `Bytes::from_owner` 持有该缓冲，发送完成后由 `encoder_free_taken_packet` 归还
reader 的共享池。池最多保留 3 个、每个 capacity 不超过 1 MiB 的缓冲；队列仍最多 16 个 AU。
归还后的 vector 保留有效长度，下一次只按实际包长 resize，不能每帧清零整个 1 MiB。
已取走的包可以比编码器活得更久，因此归还必须持有池的 shared owner，不能访问已销毁的编码器。

Core / `wait_ready_h26x_packet` 不要用 `std::condition_variable::wait_for`（musl/riscv64 会立刻返回），也不要 `FUTEX_WAIT_PRIVATE`（会丢唤醒，16 槽 reader 打满后变成 VENC 60 / Core 1 FPS）。现行等待使用 eventfd + poll，保留 deadline、40 ms 超时和 stop 检查；只有 eventfd 创建失败时才回退 1 ms 轮询。

MMF 资源策略仍为一路 H264/H265 加一路 JPEG。双 H264、双 H265 的第二次独立分配
返回 `RESOURCE_BUSY`；两个客户端订阅同一主码流属于分发测试，不能当成双硬件编码。
缓冲复用和事件等待保持 H264、H265、H264 + JPEG 的帧率与编码参数，不改变压缩码率。
性能测量须区分 MMF ABI 与网络分发：单 H26x CPU 未测到明确增益，混合编码的 H264
取包等待缩短；Core 分发和 VNC 调度的收益不能归因于硬件编码变快。JPEG 改为 fd
事件等待的独立候选没有测到明显收益，未保留。JPEG 的事务门禁和长等待锁范围见下文。

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
H.264 用 VBR（Coda 的 AVBR `MotionLv` 在 107 上一直是 0，静帧 QP 钳死、live 改
`MaxBitRate` 也不会重算 target）。H.265 仍用 AVBR：`s32MinStillPercent=90`，
`u32MaxStillQP=40`，`u32MotionSensitivity=100`，`u32StatTime=1`，
`s32AvbrFrmLostOpen=0`。
驱动 CreateChn 默认也曾是 `FrmLostOpen=1`、`MaxStillQP=1`、`ChangePos` 下限 50。
不要把 `MaxStillQP` 设成 1。
`MinStillPercent=5` / `MaxStillQP=40` 会把静帧压到 200–300 kbps，画面发糊。
H.265 走 WAVE4 固件 RC（`cviRcEn=0` / `hostPicRCEnable=0`）。静帧/运动只在目标码率或 MaxQP 真变时发 `ENC_SET_PARA_CHANGE`（`W4_CMD_ENC_RC_TARGET_RATE`）。记录见 `docs/avbr-wave4-firmware-rc.md`。

## CSIBDG 与画面宽

CSI 桥是 **精确匹配**：宽度大于设定（GT）或小于设定（LS）都会把前端打挂。

- `u32PicWidth` / VENC 宽用真实 HDMI 或目标尺寸（800 就是 800）。
- 64 对齐只用于 VB stride，不要拿对齐后的值去配 CSIBDG。
- `video.resolution=0`（自动）时，VI/VENC 跟当前 **支持的** HDMI 输入。480 对应 **640×480**，不是 854×480。
- 目标分辨率只改 VPSS/VENC。VI 接收端：HDMI 变大立刻放大；HDMI 变小要等 CSI 有效尺寸跟上。HDMI 时序空白时不要缩小。
- VI 已经停帧（`FrameRate=0`）时例外：HDMI I2C 连续给出另一个支持的模式，就跟过去重建。假 1440 把 CSIBDG 配大之后，源已经回到 1080、CSI 仍是 `0x0`，再等 CSI 会对死。
- PCIe 冷启动先触发 D283 测量，并在 VI/CSI 尚未启动、读取 `80ee` 不会破坏活流的窗口内，最多等待 2.5 秒，要求两次连续一致的合法 HDMI 时序。窗口内读到的最大合法时序和本次启动记住的活路尺寸可作候选；均不可用时才用 640×480 bootstrap。若 CSI/HDMI 连续 3 次仍全为 `0x0`，把接收端提升到 1920×1080 上限再试，处理同屏器另一端已经把源协商到 1080p、而本端读不到时序的情况。这是接收器探测，不是输入分辨率白名单。
- 同一几何也会卡住：VI 停帧、CSI `0x0`、HDMI 仍报当前尺寸时，重开 source 并在 VI 启动前重新武装 LT6911 CSI。只有在真实 AU 恢复后，才清除本次恢复状态。输出缩放只改 VPSS/VENC，不能清掉仍在等待真实 AU 的 HDMI 恢复计时。
- PCIe 正常切换参考 139 的「停采集 → 等新时序 → 重开采集」流程。VENC 停包 250 ms 后，连续两次静默读数全零才进入占位；关闭旧 MMF source 后最多等 2 秒，要求两次一致的合法新时序。不同几何立即采用；同一几何持续约 1.2 秒也可重新锁定，但必须重新武装 CSI，并用新 AU 证明恢复。普通七档切换无需 GPIO 复位。
- 等新时序时，要与**旧采集流实际配置的输入尺寸**比较，不能与 watcher 推测的下一档接收尺寸比较。139 的重开流程使用旧 CIF 宽高；107 若拿推测值比较，1280×720→720×400 时可能把桥片残留的旧 1280×720 当成新时序，随后二次重建。
- 139 只对 V4L2 CIF 执行 `STREAMOFF`、释放缓冲、重配后 `STREAMON`，宽高变化才重开 VEPU；107 的 LT6911UXC 和 MMF 没有对应的 V4L2 事件与流重配接口。107 的 `CVI_VI_DisableChn` 在断流时约等待 270–320 ms；先保留驱动停机保护，不能仅凭这段计时缩短内核等待。清理临时分段计时后，107 从 720×400 到 1920×1080 的十二档实机测试为 12/12，四角、色条、输入与编码尺寸均正确；低档首帧约 1.2–1.5 秒，1024×768 及更高约 0.7–0.8 秒。
- 对 107 正常切换路径分段计时：最低三档关闭旧 MMF source 用 558–657 ms，随后读取两次一致的新 HDMI 接收尺寸只用约 118 ms。关闭阶段包含 H.26x 编码器约 167–169 ms、VI 通道停流约 273–277 ms、ISP 停止约 51–301 ms；完整 SYS 退出约 4–5 ms。再次测试最低四档时，关旧流加确认新时序为 919–1076 ms，重开 source 只需约 70 ms（其中 MMF 初始化约 59–61 ms）。139 的 CIF 局部重开与 107 的完整 MMF 重建在资源边界上不同。若继续优化 107，重点是安全缩短旧流关闭与桥片等待，保留编码器尾帧清空和 VI 停流保护；仅跳过重开时的 SYS/VB 初始化最多节约约 60 ms，单独缩短内核停流等待也已实测无收益。
- 单独把 107 的 VENC 停包后空时序探测门槛从 250 ms 降到 100 ms，OE/kas 应用 IPK 在 107 测最低三档：候选 107／139 中位数为 2364／2159 ms，原版同档为 2360／2067 ms；三台设备的图案和尺寸均正确，但 107 没有可测提速。已撤回源码并在 107 安装原逻辑 IPK，后端 SHA-256 恢复为 `2eb50519448b32cff82b71dc8c33433d7ffa3e2f1cf91035a80a815b02bd0d3d`。报告在 `/tmp/onekvm-107-early-probe-trial-20260928/report.json`。
- 107 的 LT6911UXC 已安装第六档低时钟等待固件（整片 SHA-256 `92a0098275ef49ddceec7b66dc648f00cc2e382a4bdb7188561ee3e50227247a`），保留此前 19.904 MHz BIOS 修正、EDID 与板卡数据。写前、写后各 128 KiB 整片回读和五页逐页校验均通过。与 105／139 同源递增十二档，三台均 12/12 图案及输入／编码尺寸正确；107／139 网页首帧中位数为 1829／2117.5 ms，107 在 11/12 档更快。最低六档重复测试均 6/6，107／139 中位数为 1813.5／1951 ms，107 六档均快。107 五档输出缩放通过，断信号占位连续截图稳定并能恢复实况。原始镜像、回读和报告见 `artifacts/firmware/107-lt6911uxc-stage6-candidate-20260928.md`。这些是同源端到端时间，139 的偶发慢档不能被当作固件自身提速的独立量测。
- 图案先显示、再改变 99 HDMI 模式的端到端测试中，107／139 由 720×400 逐档升至 1920×1080 各 12/12 正确；从 `xrandr` 命令开始到网页首个正确图案的中位数为 2119／2024 ms。107 与 139 在此测法下已接近，但个别模式互有快慢，不等于每档都更快。报告在 `/tmp/onekvm-direct-switch-full-20260928/report.json`；旧测法先切模式再显示图案，不能直接与该端到端时间相减比较。
- 固定 1920×1080 输入并先确认画面稳定，再用浏览器逐帧计时五档输出缩放：107 5/5 正确，640×480、800×600、1024×768、1280×720、1920×1080 分别为 306／309／331／279／330 ms，中位数 309 ms；105 同轮中位数 258 ms。首次未等待 HDMI 输入恢复的试验把 107 的一次 9.6 秒断信号恢复误算作缩放延迟，测试套件现已加入输入稳定门槛。报告在 `/tmp/onekvm-output-precise-stable-20260928/report.json`。
- PCIe 在旧 VI 已停止的恢复窗口可读取 LT6911UXC 的 HDMI 接收侧 `0x86:7e/80` 尺寸，两次一致后用它配置新 VI；活动画面仍不读取。720×400 实测该尺寸在模式命令结束后约 0.66 秒出现，但 `0x86a3` 仍为 `0x88`，CSI 到约 3.03 秒才给出尺寸、约 3.19 秒才报锁定，因此早期尺寸只用于准备接收端，不能当作已有图像。采用这一流程后，同源十二档输入四角、色条与尺寸 12/12 通过；从测试图案就绪到网页首帧的中位数为 807 ms，139 同轮为 705 ms。固定 1080p 输入的五档输出缩放 5/5 通过。关闭 99 的 HDMI 输出并暂停其自动显示配置后，连续八次截图均为占位图，没有旧桌面残留。原始报告在 `/tmp/onekvm-139-flow-rx-full-20260928/report.json` 与 `/tmp/onekvm-107-rx-output-20260928/report.json`。
- 139 的 TC358743 `SOURCE_CHANGE` 事件和 `QUERY_DV_TIMINGS` 可安全连续读取；107 的 LT6911UXC 没有等价事件，完整时序读取会打开 `80ee`，反复读取低像素时钟模式可能使 CSI 失锁，静默读取在停 VI 后又可能一直报零。不能把 139 的四次时序采样或事件触发直接搬到 107。实机尝试提前使用两次空白读数启动重开后，720×400 的画面虽短暂出现，下一档却停在旧尺寸占位；增加到四次完整采样仍复现，改用静默采样则低档生效需 4.7–10.6 秒。当前两次完整采样加已验证的恢复门槛通过了低档回归。
- 对齐 139「锁定新时序后直接启动采集」的步骤：107 PCIe 仅在正常停流并连续两次读到相同的新 HDMI 接收尺寸后，重开时复用该时序，不再额外执行 `kick_hdmi` 或在 CSI 启动时写 `D283`；冷启动、同尺寸失锁和无信号恢复仍保留重新测量。107 与 139 同源十二档输入切换均为 12/12 正确，107 固定 1080p 输入的五档输出缩放为 5/5；107 断信号后显示干净占位图，恢复输入后同一浏览器会话显示实时画面。107 最低六档网页首帧中位数从 1167 ms 降至 1106 ms；同轮 139 为 843 ms，LT6911UXC 在低档发布新时序及 CSI 出帧仍较慢。报告在 `/tmp/onekvm-139-flow-comparison-current/report.json`、`/tmp/onekvm-107-cached-timing-trial/report.json` 和 `/tmp/onekvm-107-cached-timing-output/report.json`。
- 后续将正常切换路径中与 `D283` 配套的 100／50 ms 等待一并跳过，冷启动和恢复路径不变。105／107／139 共用 HDMI 输入，由 720×400 逐档升至 1920×1080，三台各 12/12 通过四角、色条和输入／编码尺寸检查；首帧中位数分别为 1788／618／689 ms。107 最低四档为 1174／875／916／977 ms，139 同轮为 1008／525／816／544 ms；低档差距仍在。107 五档输出缩放 5/5 通过，断信号后的占位连续截图稳定；恢复 HDMI 后，107 和 139 均重新识别到同一测试图案。报告在 `/tmp/onekvm-three-no-cached-settle-full-20260928/report.json`、`/tmp/onekvm-107-no-cached-settle-output-20260928/report.json` 和 `/tmp/onekvm-107-after-disconnect-live-20260928/report.json`。
- HDMI 持续为零时保留原接收尺寸和占位编码尺寸，等待输入回来，不为探测而轮换 640×480／720×400，也不在无输入时打 GPIO。占位已解开 VI→VPSS，此时可用 `80ee` 完整时序读取发现重新接入的 LT6911UXC：断信号后，静默读数可能仍是 `0×0`，而完整读取已报告新的 HDMI active size。读到合法时序后软件重开 source；只有时序已出现、但重开后仍没有真实 AU 的情况，才按冷却时间使用 GPIO 复位兜底。完整复位需独占 GPIOB3，低电平 1 秒、拉高后至少 1 秒才探测；设备树不能为该线设置 `gpio-hog`。
- Cube 没有可控的 LT6911 硬件复位线。空白失锁时只关闭旧 MMF source，在 VI 启动前重新测量 HDMI、武装 CSI，再重建 VI/VPSS/VENC；不要套用 PCIe 的 GPIO 和固件等待时间。
- 同源七档 720×400 至 1920×1080 实机验证，105／107／139 均识别四角、色条、输入状态和编码尺寸（21/21）。网页生效时间中位数分别为 2.688／1.914／1.184 秒。107 固定 1920×1080 输入的五档输出缩放全部通过，网页生效中位数 0.750 秒。107 断输入后占位图稳定，输入恢复后的同一浏览器会话约 2 秒重新显示实时画面；日志确认该轮未执行 GPIO 复位。历史短复位测量保存在 `local-docs/`。
- 720 宽度的 VI YUV 旁路行距按 16 字节对齐，VPSS online 输入必须使用同一行距；按 64 字节计算会让 720×400/480 逐行错位。修复后 107 实测 720×400@70、720×480@60 及其余 640×480 至 1920×1080 输入，四角与色条均正常。
- 不要只信 HDMI I2C，也不要只信 CSI 当前宽。只信 HDMI 会在 MIPI 仍是 1920 时把 VI 配成 800；只信 CSI 会在 800→1080 时把接收端留在 800。
- CSI 已是合法模式时，忽略 I2C 垃圾 OOR 读数。

HDMI 输入不是固定模式白名单：接受 `320×200` 到 `2880×1620` 范围内、不超过 5 MP 的偶数尺寸，并按 **5 MP @ 30 FPS**（150M pixel/s）吞吐预算判定。4K 时序仍会被识别，但无论帧率均报告 `out_of_range`，不得用于重建 VI。

## HDMI 探测与 LT6911

HDMI 重建和 LT6911 CSI 时序归 **watcher**。不要读 `/proc/cvitek/vi` 判断热插拔。

- 活路（已绑定消费者、`cached_signal==1`）完全跳过 LT6911；HDMI 在不在看 VENC 包。即使 1 Hz `80ee` 最终也会拖死 CSI。
- 空闲时保持 LT6911、VI 和 VI→VPSS 已配置，但不读 LT6911 I²C；状态查询只返回缓存。消费者接入后先直接恢复 VPSS/VENC，任何分辨率只要 VENC 正常出包都不得探 LT6911。模式切换或无信号先表现为 VENC 停帧并切换占位，随后 watcher 才解析 LT6911 CSI（`0xc238`/`0xc206`）并恢复。这样会把空闲期间发生的模式切换延迟到下一个消费者，但不会在消费者到来前用状态探测打停低时钟 CSI。
- 绑定编码器实际停帧后再进入恢复探测。
- 半速锁例外：目标 ≥48 FPS 而 VENC `EncFramePerSec` 连续约 2 s 钉在一半（1080p60→30，720p120→60）时，AU 仍在 1.5 s 存活窗口内，watcher 不会探。用已有 2 Hz `venc` proc 的 `EncFramePerSec`，不要读 `vi`/`vi_dbg`。确认后 `reopen_source`（先 teardown 再 `lt6911_start_csi`）。同一目标帧率只重装一次，直到帧率回到目标附近；真 1080p30 源会闪一次然后停。

开机 `open_source` 分两段写 LT6911，都在 **VI init 之前**。Core 重启不会重跑 `prepare-hdmi`，所以这是 MMF 的职责。不要合成「先 D283 再 805a」的单次 80ee 会话。实际顺序必须是：只读缓存 LT6911 已锁定的合法时序 → 回收上一进程的 VI owner → 必要时 D283 测量并确定输入几何 → `lt6911_start_csi()` → 回收遗留 VB/VENC/VPSS → MMF SYS/VB 与 `SAMPLE_PLAT_VI_INIT()`。不能先启动 VI 再 arm CSI；残留的 1080p CSI 会立刻打中 640p bootstrap 的 CSIBDG 精确宽度检查。

PCIe 例外：VI 尚未启动时若 CSI 已报告稳定、受支持的 active size，直接保留固件现有锁定并启动 VI，不再执行 GPIO reset、`kick_hdmi` 或 `lt6911_start_csi`。这用于固件已经接受、但重新 arm 会丢锁的低时钟模式。只有 CSI 未锁定或请求几何与 CSI 不一致时才走下面的恢复序列。持久化 EDID 在写入时已经更新桥片；创建 source 不得再次无条件恢复 EDID并连做两次 GPIO reset。

1. 先只读一次 LT6911 时序，再由 `mmf::reclaim_stale_runtime()` 回收上一代 VI owner。只在发现遗留 VB pool 时执行 vendor VI teardown，而且每个进程最多一次；旧 VI 尚未清理时重编程 CSI 会在活 HDMI 上锁死 vendor 前端。底层 VB/VENC/VPSS 强制回收仍留在 `initialize_runtime()`，不能提前到 `CVI_SYS_GetVersion()` / `CVI_LOG_SetLevelConf()` 之前，否则会使同一进程的 `libsys` SHM 映射失效。
2. `lt6911_kick_hdmi()`：开 `80ee`，写 `D283=0x11` 启动测量。只在打开 source 时做一次，不要从活 watcher 重复。启动探测可在 750 ms 截止时间内读时序，得到连续两次一致值后立即停止。
3. 冷启动的 `lt6911_start_csi()` 与 `prepare-hdmi` 相同：**先** `0x805a=0x80`、`0x8010=0x00`，等 100 ms，**再** `D283=0x11`，再等 50 ms，最后关 `80ee`。PCIe 正常模式切换已确认新时序时，使用 `lt6911_start_csi_with_cached_timing()`，直接关闭 `80ee` 并启动 VI；跳过重复 `D283` 及其前后 100／50 ms 等待。
4. `mmf::initialize()` 创建 SYS/VB 并执行 `SAMPLE_PLAT_VI_INIT()`；只有到这一步才启动 VI。空闲 `DisableChn` 之后再次 `EnableChn` 也要先走第 3 步。

输入几何必须在第 3 步之前确定。`StartViChn` 后不要为了发布状态再调用 `read_hdmi_input()`；低时钟模式会在第一次消费者到来前被这次 `80ee` 读取打停。启动后沿用已配置的输入几何，真正停帧后再由恢复 watcher 更新。

MMF 初始化失败的回滚按所有权执行：`SAMPLE_PLAT_VI_INIT()` 负责清理它已经取得的 sensor/dev/ISP/SYS 资源，外层不得再调用一次完整 `DestroyVi`。`shutdown_vendor_system()` 只释放成功取得并记录为 owned 的 VI 和 SYS；否则 `/dev/vi` 的 release 会对从未 prepare 的 `clk_csi_mac0_vip` 再做一次 unprepare。

不要在 `StartViChn` 之后打 `0x805a=0x88`（CSIBDG 精确匹配，TX 掉到 0 会把 IntCnt 打成 0）。`reboot -f` 只复位 SoC，不复位 LT6911。

## 无信号占位

绑定路径不能在 HDMI 丢失后把同一个 VENC 通道改成 `SendFrame` 占位：`VPSS_UnBind` 不会清掉厂商驱动的 `currBindMode`，随后直送帧会和 `venc-handler` 在全局 VPU 锁上互锁，并耗尽 VPSS VB pool。

无信号期间保持 VPSS→VENC 绑定，只解开 **VI→VPSS**，把 NV21 占位转成 UYVY 后 `CVI_VPSS_SendFrame` 送到 group 0。WAVE4 按平常绑定路径出 IDR/P。输入恢复后重新 `VI_Bind_VPSS`。前端控制台不要叠 Vue 占位图。

`StartRecvFrame` 发生在 1.5 s live grace 里。无 HDMI 时 WAVE4 随后把静帧编成 P/skip，reader 若仍在等「自然第一帧 IDR」会把 `RequestIDR` 合并掉，WebRTC 拿不到可解码的 AU。进入占位后必须 `h26x_reader_force_idr`（清队含残留关键帧，并在 reader 线程发 ioctl），且在匹配当前 VENC 宽高的 IDR 到达前不要把 AU 交给 Core。H.264 再按 SPS 尺寸丢掉残留 CSI 包。

超过几何范围或 150M pixel/s 吞吐预算的 HDMI 模式仍在 status 中提供 `hdmi_error=out_of_range` 和实测 `input_width/height`，UI 显示「不支持的分辨率」；绑定视频同样走 VPSS 上游静帧。

VI `FrameRate` 列开机约 1 秒是 0，不能单靠这一列判无信号。已经收到过 AU 后，判定输入消失仍要等 VENC 最近一包超过存活窗口（当前 1.5 s）；新绑定或从空闲恢复后使用独立的 5 秒首帧窗口，避免慢锁定被误判成无信号并触发 LT6911 恢复。

## 空闲与释放

无消费者时 Core `videoLoop` 停在 `waitForConsumer`。空闲只 `CVI_VPSS_DisableChn` 停 scaler（`CVI_VIP_SCL`），保持 VI→VPSS 绑定和 VI DMA。`CVI_VI_DisableChn` / UnBind 会把 Preraw 打停，后续 `EnableChn`+`SetChnAttr`+`lt6911_start_csi` 仍拉不回 CSIBDG，活路会掉进无 HDMI 占位。保留 VPSS→VENC、VENC bind worker 和唯一的用户态 VENC reader；reader 进入 discard 模式，以有界 fd poll 排空停 scaler 后的迟到 AU。直接解绑而不先排空在途帧，会让无输入的 worker 在最终 `StopRecvFrame` 时卡进厂商锁。重新绑定先 `EnableChn` VPSS。

`unbind_h26x_from_capture` 只断开 SYS bind，不停 VI。空闲不要 `DisableChn`：在 SG2002 HDMI 上它不可逆。不要因为 CSI `0x0` + HDMI 仍是 1080 就 `reopen_source`（空闲停 scaler 和占位都会出现这个读数）。从空闲恢复只 `resume_vpss_channel`。

重新绑定 VENC 时先清除旧的 `last_packet_ns` 和 `read_error`，再退出 discard 并强制 IDR；这样首帧沿用 `bound_since_ns` 的 1.5 秒 grace，不会把空闲前的旧时间戳误判为 VENC 停帧并切换 placeholder。Enable 失败则保持关闭并让 Core 重试，不要拆 VI。

最终关闭仍由同一个 MMF teardown 执行。不能先停 reader 再取消正在编码的 bind worker；驱动的 safety reset 可能让 Coda9 在下一进程执行 sequence init 时仍为 busy。`close_h26x_encoder` 的顺序是：

1. 若仍绑定且 scaler 已停，先恢复 VI DMA / VPSS，让已经进入厂商 `GetStream` 的 reader 有机会退出锁
2. reader 进入 discard，清掉用户态队列，然后 `pause_vpss_channel` 停止新输入
3. 保留唯一 reader 排空最后一个在途 AU；等待有界静默期后 join reader
4. Unbind（不停 VI DMA），使驱动的 `enable_bind_mode` 变为 false
5. 调用一次 `StopRecvFrame`，唤醒并 join 已空闲的 bind kthread，清掉 `currBindMode`
6. `ResetChn` / `DestroyChn`

不要在 SYS binding 仍启用时先调用 `StopRecvFrame`：这只会把 channel 标成 STOP，既没有回收 bind kthread，也扩大了最后一帧与 reader 退出的竞态。
不要在 `finish_idle_h26x_drain` 里 join：DisableChn 之后仍可能有迟到 AU，唯一 reader 必须继续排空。

驱动侧 bind worker 的生命周期由 `osdrv-sg200x` 源码处理（显式 task 引用、completion、Destroy 前 join）。
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

截图走 v1 ABI 的可选 `source_snapshot`：复用已有 source/VPSS，申请受管 JPEG encoder，直接提交 VPSS VB 物理帧，把 JPEG 复制到调用者缓冲区后释放。截图不映射或 invalidate 整幅 NV21。
Core 只做鉴权和成品 JPEG 转发。扩展不得创建第二个 MMF source，也不得在绑定串流路径调用 `source_read`。
显式截图尺寸必须匹配当前输出，不能为采样重设实时流分辨率。

绑定 VPSS 输出保持 `Depth=1`，供低频截图获取最新完成帧。vendor 的满队列处理会释放旧帧、保留新帧；空闲时 DisableChn 清空队列。
帧必须保持借用直到 JPEG stream 已释放；失败时先排掉已提交结果、ReleaseStream，再 Stop/Destroy JPEG 和归还 VPSS 帧。原本暂停的 VPSS 在截图结束后恢复暂停。
请求期限覆盖取帧和 JPEG 查询/送帧，最长配置 1000 ms；vendor JPEG GetStream 忽略传入 timeout，实际硬件等待由驱动控制，不能视为严格的调用时限。结果 PTS 是 backend 单调时钟观察时刻，不是曝光时间。

已确认无信号或 `out_of_range` 时立即返回 `UNSUPPORTED`，不要用占位图制造 JPEG，也不要在占位 `SendFrame` 还握着 source 锁时去 `GetChnFrame`。

当前 policy 允许一路 H.26x 与一路 JPEG，不能据此断言芯片只能一路 H.26x。
`BACKGROUND` 用途不自动抢占实时 encoder；KVM 空闲也不表示 encoder 已销毁。后台合成需要 Core 的显式所有权交接。

## MJPEG 与 VNC

VNC 订阅 Core 的常驻逻辑绑定 JPEG encoder，复用同一 source 和 VPSS ch1，
不逐帧截图，也不从 WebRTC H.264 解码。JPEG 客户端接收原生 encoding 21 或
Tight JPEG；Core 将压缩包写入共享 ring，VNC 借用 ring 槽并直接写 socket。
仅 RGB 客户端需要像素解码和脏块检测，兼容路径最多 15 FPS。

- JPEG 硬件输入借用 VPSS VB descriptor/物理地址，不映射或 invalidate 整幅
  NV21。CPU 读取像素时才使用原有映射接口。VB 必须保持借用直到 JPEG 完成；
  错误路径先排流、释放 stream 和停止 JPEG 通道，再归还帧，不能为缩短锁等待提前释放 VB。
- 流式 JPEG 的 2 MiB 压缩输出缓冲通过共享池回收，池最多缓存三个空闲缓冲。
  vector 的可写长度与实际 JPEG 包长分别保存，不能逐帧缩短再清零整个缓冲。
  已交出的包可比 encoder 活得更久，包的 owner 必须持有 pool，避免 reset/close
  后回收访问已析构对象。驱动到 backend、Core 到 ring 仍有压缩码流复制。
- JPEG 数据事务由 `g_mmf_transaction_mutex` 独占。锁顺序为 encoder →
  source → transaction → global；控制操作取得 transaction 后才取得 global。
  bound JPEG 配置和恢复 VPSS 后释放 source，取帧、编码等待和归还只持
  transaction；事务完成不重新取得 source，避免控制线程等待事务时死锁。
  shutdown、重建、截图和手动编码都受同一事务门禁保护。
- VPSS GetFrame 成功立即登记 held；映射失败也走统一归还。Destroy 或
  Release 失败时保留描述符、VB 和待释放状态，下一次读取/截图/绑定先重试。
  手动 JPEG 借用的 raw 帧须等 `source_release` 后归还，不能由 JPEG teardown
  提前回收仍由 CPU 使用的帧。未完成清理时禁止销毁 capture pool/SYS。
- vendor JPU 在 SendFrame 成功后持锁，GetStream/ReleaseStream 才解锁，
  DestroyChn 会再次取得同一锁。QueryStatus/GetStream 失败后不能直接销毁：
  先尝试排掉已提交结果，未完成则保留 runtime 的 cleanup_pending，禁止同尺寸
  快捷复用旧通道或修改编码器配置。删除 ABI owner 时清除 owner 指针，硬件隔离
  状态继续留在 runtime。真实硬件超时的 vendor 取流错误分支存在遗漏解锁，
  此时保留资源可避免过早释放，但不能承诺无需驱动修复即可恢复。
- H26x bound owned 包已由唯一 reader 归还 vendor stream，ABI release 的
  软件清理不取得 global；borrowed/manual 包仍按硬件所有权释放。坏包、空包
  和分配失败后保留未归还 stream，排空并请求 IDR，恢复时跳过依赖旧参考链的帧。
- Core 的 MJPEG 帧尺寸取本帧 JPEG SOF，不使用 HDMI 输入尺寸或取包后再查的
  当前尺寸；输出缩放和切换中的旧帧都必须携带匹配的元数据。读取帧头不解码像素。
- 主编码器的实际 codec 在安装、reset 和所有权恢复时缓存。MJPEG/status 的
  codec 查询不得等待 H.26x 的阻塞取帧锁。
- 关闭 WebRTC 会话与释放主 H.26x 硬件通道不是同一动作；JPEG 仍依赖现有
  VI/VPSS 采集。不能绕过 Core 的需求管理和后端拆卸顺序，直接销毁主通道。

## 帧率、通道与 carveout

- 采集一律走 phy chn 1（`sc_v1`，最大输出宽 2880）。phy chn 0（`sc_d`，最大 1920）上 1:1 2560 会 tile，NV21 全 0。HDMI 1080→1440 同一 PID 不换通道。
- VPSS 与 VENC 使用相同目标帧率，不再人为保持 60/30 FPS floor。720p 高刷时 `input_fps`/`TarFr` 可到 120。
- Core 绑定路径的 RTP 时戳用 `1/目标FPS`，不要用墙钟间隔。
- VPSS 和 VENC 都按输入、输出两端较严格的像素率限制帧率，避免小输入放大到 1620p 时 scaler 仍跑 60 FPS。
- 显式最大输入/输出为 **2880×1620@30**（4.67 MP、约 140 Mpixel/s），使用 **64 MiB** 视频 carveout：高于 2560×1440 时 VI 公共池两块 UYVY，VPSS 私有池仍三块。
- 720p@120 需要源真出 1280×720@120（CEA VIC 47）；Cube 默认 EDID 不含该模式。
- 活着的受支持模式不要探 LT6911 I2C。
- CSI 半速（活包仍在、帧率是目标的一半）按上面 HDMI 节重装，不要当无信号占位，也不要为查帧率去 dump `/proc/cvitek/vi`。

## SRTP CryptoDMA

`write_sample` 在视频线程上同步 `block_on` 加密。完成位是 `CRYPTODMA_WR_INT`，不要 `wait_event` 活 DTB 上那条从不触发的 PLIC 59。
单次 `ETIMEDOUT` 只让当前批次走软件；Core 以 1、2、4、8、16、30 秒（封顶）
指数退避自动探测恢复。不要因一次瞬态超时在进程生命周期内永久禁用 offload。
不要给 crypto backend 打 `CONCURRENT_H265_VIDEO`：H.265 VENC 和 CryptoDMA 同时跑会锁死 SG2002，Core 对 H.265 会话改用软件 AES-GCM。

## WebSocket 分发

Core 为同一 AU、相同元数据的订阅者只序列化一次 OKVF 消息，共用不可变
`Bytes`。每个客户端有独立的 64 条消息队列，正常消费者保留连续 AU；落后
越过队列后请求 IDR，并等待关键帧再发送。不能从任意最新 P 帧继续。
双客户端仍分别传输码流，不减少网络副本数。共享全局 sequence 可能混入
JPEG，不能把其跳号直接记为 H26x 丢帧；验证须保存各客户端码流实际解码。

VNC 前台由 frame/audio/eventfd 和 socket 事件唤醒，空闲轮询上限 20ms。
Native/Tight JPEG 每客户端保留一个在途 owner，通过非阻塞 sendmsg 分段发送；
超过 2 秒仍未写完关闭对应客户端。在途消息期间推迟同连接的库回复和音频，
PCM 最多延期 500ms，超过预算关闭该客户端。resize 等全部在途 JPEG 完成或
超时，仍可能被慢客户端拖延最多 2 秒；Raw/ZRLE 库发送仍为同步。
