# V4L2 视频后端（soph-v4l2.so）

[English](../en/v4l2-backend.md)

`soph-v4l2.so` 用内核 V4L2 节点实现 `src/mmf.hpp` 的 `onekvm::mmf` 接口，上层
（ABI、编码器、截图、LT6911、EDID、无信号画面）与 `soph-mmf.so` 共用同一份源码。
它只链接 libc、libstdc++ 和 libgcc，不加载 cvi_mpi，也不需要
`onekvm-video-soph-mmf-runtime`。

状态：实验实现。设备描述符默认加载 `soph-mmf.so`。

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
| 跳帧 | 有码率上限（手动码率或自动码率给出的上限）时开启 `FRAME_SKIP_MODE` 缓冲上限 |

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

主编码为 MJPEG 时，最后一个 MJPEG 客户端断开后，上层对主 JPEG 编码器解绑来源，后端
随即暂停 VI 并停下缩放通道。此前这一步不起作用，两个后端一直保持采集缓冲，直到改用
H.26x 或服务重启。VNC 逻辑绑定的 JPEG 编码器解绑时不暂停采集。

## 码率上限

H.264 使用 VBR，码率上限从下一帧生效。H.265 使用 AVBR，每个统计周期更新模型。编码器达到 QP 上限时，可以用跳帧限制输出码率，代价是降低编码帧率。
