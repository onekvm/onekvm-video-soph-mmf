# V4L2 视频后端（soph-v4l2.so）

[English](../en/v4l2-backend.md)

`soph-v4l2.so` 用内核 V4L2 节点实现 `src/mmf.hpp` 的 `onekvm::mmf` 接口，上层
（ABI、编码器、截图、LT6911、EDID、无信号画面）与 `soph-mmf.so` 共用同一份源码。
它只链接 libc、libstdc++ 和 libgcc，不加载 cvi_mpi，也不需要
`onekvm-video-soph-mmf-runtime`。

状态：实验实现，已在 107（6.18.57-onekvm-preempt）完成功能、延迟、资源与内存验收。
设备描述符默认仍加载 `soph-mmf.so`。

## 节点与依赖

| 用途 | 节点 | 文档 |
| --- | --- | --- |
| HDMI 采集，直接输出 NV21 | `soph-vi` | [v4l2-capture.md](v4l2-capture.md) |
| 缩放（输出尺寸与 HDMI 不同时） | `soph-vpss` | [v4l2-scaler.md](v4l2-scaler.md) |
| H.264 / H.265 | `soph-venc` | [v4l2-encoder.md](v4l2-encoder.md) |
| JPEG（MJPEG、截图） | `soph-jpegenc` | [v4l2-encoder.md](v4l2-encoder.md) |

须与注册这些节点的 `soph_*` 模块一起安装；旧模块没有这些节点，后端初始化会失败。
LT6911 仍由用户态经 i2c-dev 控制（`src/lt6911_bridge.c`）。

## 选择后端

两个后端由同一配方构建，安装在 `/usr/lib/onekvm/video-backends/`。Core 加载
`/usr/share/onekvm/device-backends/nanokvm.json` 中 `library` 指定的库：

```json
"library": "/usr/lib/onekvm/video-backends/soph-v4l2.so"
```

改完重启 `onekvm.service`。两个后端不能同时运行：VI 由先打开的一方独占。

CMake 选项：`ONEKVM_BUILD_V4L2_BACKEND`（默认 ON）构建 `soph-v4l2.so`；
`ONEKVM_BUILD_MMF_BACKEND`（默认 ON）构建 `soph-mmf.so`，只有它需要
`CVI_MPI_ROOT`。

## 数据通路

```text
LT6911 ─CSI─▶ soph-vi (NV21, MMAP, 导出 DMA-BUF)
                 ├─ 同尺寸通道 ─────────────▶ soph-venc / soph-jpegenc (OUTPUT DMABUF)
                 └─ 缩放通道 ─▶ soph-vpss ──▶ soph-venc / soph-jpegenc
```

- 线程：控制线程（上层调用，经 `MmfControlLock` 串行）；一个 pipeline 线程把
  VI 帧分发到采集通道、绑定的编码器和最新帧槽；每个 H.26x 编码器一个 reader
  线程，独占取码流。
- 帧以 DMA-BUF 在节点间传递，不经 CPU 拷贝。每个编码器最多同时排入 2 帧；新帧
  到达时编码器仍忙则丢弃该帧并计数，不排队增加延迟。
- VI 帧结束时间戳经 `soph-vpss` 一直传到编码输出，延迟面板的“采集”项 = 帧周期 +
  （编码器取到帧 − VI 帧结束）。
- 无信号时，pipeline 以 VI 尺寸的 staging 缓冲送入占位画面，编码器配置不变。

| 缓冲 | 数量 | 生存期 |
| --- | --- | --- |
| VI NV21 | 6（单帧大于 4 MiB 时 4） | 只在 VI 运行时分配，暂停即释放 |
| VPSS 输出（缩放通道） | 4（单帧大于 4 MiB 时 3） | 只在通道运行时分配 |
| H.26x 码流 | 4 | 编码器打开期间 |
| JPEG 码流 | 2 | JPEG 编码器打开期间 |

## 编码参数

H.264 与 H.265 的码率控制沿用 cvi_mpi 运行时的取值：

| 项 | 取值 |
| --- | --- |
| 码控 | H.264 VBR；H.265 AVBR |
| VBR/AVBR 细项 | ChangePos 90，I/P 比 1–100，MaxIQp = MaxQp − 2，IPQpDelta 2 |
| 默认 QP | 最小 12，最大 48，初始 22 |
| 运行中修改 | 帧率/GOP、码率、QP 由 reader 线程在帧间应用；IDR 用 `FORCE_KEY_FRAME` |

静态画面下 `soph-venc` 输出跳过块（P 帧约十几字节），cvi_mpi 运行时每个 P 帧约
9.6 KB；两者解码结果相同。

JPEG 直接编码借出的采集帧（同一 DMA-BUF），只有调用方传入的 CPU 内存才拷入
staging 缓冲。

## 空闲与内存

VI 暂停时：

1. VI `STREAMOFF` 后释放全部 VI 缓冲；缩放通道关闭其 `soph-vpss` 上下文。
2. 空闲的编码器（绑定且已停止送帧，或未绑定且没有待取的包）对 OUTPUT 队列
   `STREAMOFF` 并 `REQBUFS(0)`，释放已导入的 DMA-BUF。否则 vb2 一直持有最后导入的
   缓冲，已“释放”的 VI 内存不会归还。
3. OUTPUT 停止即结束 VENC 会话；下次送帧时重新 `STREAMON`，新会话从 IDR 开始。
   重连本来就请求 IDR，码流语义不变。

107 实测 ION（lazy-cma，1080p60 H.264）：

| 状态 | soph-mmf.so | soph-v4l2.so |
| --- | --- | --- |
| 空闲 | 0.3 MB / 1 块 | 0 |
| 1080p60 推流 | 39.2 MB | 32.5 MB / 12 块 |
| 推流结束后空闲 | 8.3 MB（VENC 缓冲） | 0（连续 3 轮） |

MJPEG 客户端断开后上层不暂停 VI，两个后端都保持采集缓冲（soph-mmf.so 32.2 MB，
soph-v4l2.so 24.9 MB），直到改用 H.26x 或服务重启。

## 107 实测

条件：107（SG2002，6.18.57-onekvm-preempt），被控机 99 输出 1920×1080@60，
`onekvm.service` 单客户端，未开浏览器时测 CPU。两个后端用同一配方产物，切换描述符
`library` 后重启服务，交替进行。

### 端到端延迟

边界：HTTP 触发被控机全屏页面黑白翻转 → 99 显示 → HDMI → 107 → WebRTC →
无头 Chromium 轮询已解码画面（2 ms）检测到翻转。同一 Chromium 单调时钟计时，
不涉及跨机时钟。每轮 40 个有效样本，单位 ms。

| 输出 | 后端 | 轮×样本 | 视频 P50 | P95 | 均值 | 键盘闭环 P50 / P95 |
| --- | --- | --- | --- | --- | --- | --- |
| 1920×1080（直出） | soph-mmf.so | 3×40 | 58.8 | 62.3 | 58.6 | 59.2 / 72.7 |
| 1920×1080（直出） | soph-v4l2.so | 3×40 | 58.7 | 62.3 | 58.6 | 58.3 / 62.2 |
| 1280×720（缩放） | soph-mmf.so | 5×40 | 48.9 | 61.5 | 52.6 | 59.3 / 62.6 |
| 1280×720（缩放） | soph-v4l2.so | 3×40 | 59.3 | 62.3 | 55.5 | 60.2 / 63.6 |

- 样本分约 47 ms 与约 60 ms 两档（取决于帧完成时刻与显示刷新的相位），P50 落在
  哪一档取决于低档占比，比较时看均值和占比。720p 低档占比 soph-mmf.so 52%、
  soph-v4l2.so 32%，均值多 2.9 ms；1080p 两者相同（0% 对 4%）。
- soph-mmf.so 1080p 的键盘闭环 P95 受其中一轮停顿影响（最大 351 ms），视频 P95
  不受影响。
- 缩放通路多出的时间来自 VI → `soph-vpss` → `soph-venc` 之间两次经 pipeline 线程
  转交。VPSS 本身不慢：从排入到完成 6.44 ms，其中硬件 6.19 ms。把 pipeline 线程
  提到 nice -20 后 3×40 均值 57.2 ms，无改善，未保留。

Web UI 性能面板的设备侧分项两个后端口径不同，只作参考（soph-v4l2.so 对
soph-mmf.so）：1080p 采集 17 对 23 ms、编码 9.6 对 9.1 ms；720p 采集 24 对 23 ms、
编码 4.8 对 4.2 ms。

### 资源

运动画面（被控机 `?motion=1`），1080p60 H.264 WebSocket，不开浏览器；单核
SG2002，进程 CPU 以一个核为 100%，每轮采样 15 s，两后端交替各 3 轮：

| 指标 | soph-mmf.so | soph-v4l2.so | 变化 |
| --- | --- | --- | --- |
| 进程 CPU | 13.8 / 13.9 / 13.8% | 12.0 / 11.7 / 12.3% | −1.8 个百分点（−13%） |
| 实收帧率 | 60.0 FPS，无断档 | 60.0 FPS，无断档 | 无 |
| 码率 | 4.09 / 3.60 / 4.06 Mbps | 1.28 / 1.10 / 1.14 Mbps | 运动画面未逐帧对比画质 |
| RSS | 约 33 MB | 约 35–36 MB | +2–3 MB |

码率不同，CPU 差值包含 Core 发送量的差异。

### 起播与截图

H.264 WebSocket 起播到首个关键帧，及推流停止 12 s 后的单张 JPEG 截图，
1080p 与 720p 各 3 轮：

| 项 | soph-mmf.so 中位数（范围） | soph-v4l2.so 中位数（范围） |
| --- | --- | --- |
| 起播 | 0.225 s（0.21–0.25） | 0.215 s（0.20–0.25） |
| 空闲截图 | 0.331 s（0.305–0.353） | 0.308 s（0.288–0.481） |

### 功能

| 项 | 结果 |
| --- | --- |
| H.264 1080p60 WebRTC（Web UI） | 60 FPS |
| H.265 WebSocket | 60 FPS，码流解码无错 |
| MJPEG | 8 s 464 帧（soph-mmf.so 479） |
| 720p 缩放输出 | 60 FPS，1280×720 |
| 推流中 HDMI 1080p → 480p → 无信号 → 1080p | 无断流，各段尺寸正确，占位画面正常 |
| 10 次起停（1080p/720p 交替） | 每次 60 FPS，fd、ION 不增长 |
