# SG2002 V4L2 编码接口

[English](../en/v4l2-encoder.md)

`soph_vc_driver` 注册两个 V4L2 内存到内存（m2m）编码节点，直接调用内核内的
VENC 通道层，不经过 cvi_mpi。它们是后端脱离 cvi_mpi 的编码部分；采集与缩放节点
另行实现（采集见 [v4l2-capture.md](v4l2-capture.md)）。现有 MMF 流水线（字符设备 + cvi_mpi）保持可用，两者可同时运行。

状态：实验实现，已在 107（6.18.57-onekvm-preempt）完成编码、并发和码流解码验收。
后端 `soph-v4l2.so` 使用这两个节点，见 [v4l2-backend.md](v4l2-backend.md)。

## 节点

| 节点名（`/sys/class/video4linux/videoN/name`） | CAPTURE 码流 | OUTPUT 原始格式 | 尺寸 |
| --- | --- | --- | --- |
| `soph-venc` | `H264`、`HEVC` | H.264：`NV12`、`NV21`；H.265：另加 `UYVY`、`YUYV` | 64×64 至 2880×1620，偶数 |
| `soph-jpegenc` | `JPEG` | `NV12`、`NV21`、`UYVY`、`YUYV` | 64×64 至 4096×4096，偶数 |

`videoN` 编号动态分配，按节点名或 `VIDIOC_QUERYCAP` 的 `card` 查找。两个节点都是
单平面 MPLANE（`num_planes = 1`）；NV12/NV21 的色度平面紧接亮度平面，步长相同。

## 会话

1. 先 `S_FMT` CAPTURE 选择码流格式（OUTPUT 可选格式随之变化），再 `S_FMT` OUTPUT
   设置尺寸和原始格式。OUTPUT 步长至少 `ALIGN(宽 × 每像素字节, 32)`；
   可传入更大且 16 字节对齐的步长（例如 VI 写出的 `ALIGN(宽 × 2, 16)`）。
2. `S_PARM`（OUTPUT）设置输入帧率，码率控制随之更新。
3. 两个队列都 `STREAMON` 后才占用 VENC 通道并开始编码；任一队列 `STREAMOFF`
   或关闭文件即释放通道。新会话从 IDR 开始。
4. 每个 OUTPUT 缓冲产生一个 CAPTURE 访问单元（H.26x 为完整 Annex B，IDR 前带参数集；
   JPEG 为完整图像）。CAPTURE 带 `KEYFRAME` / `PFRAME` 标志，时间戳复制自 OUTPUT。
5. `V4L2_ENC_CMD_STOP` 排空已排队的输入，最后一个 CAPTURE 带 `LAST` 并发出
   `V4L2_EVENT_EOS`；`V4L2_ENC_CMD_START` 继续，下一帧为 IDR。

单帧编码失败时该 OUTPUT 以 `ERROR` 返回、CAPTURE 不出队，驱动复位通道并强制下一帧
IDR。复位失败后会话不再编码，需重开会话。

H.26x 同时只允许一个 V4L2 会话（第二个 `STREAMON` 返回 `EBUSY`）。V4L2 从最高通道号
向下占用 VENC 通道；cvi_mpi 已打开的通道不会被占用，V4L2 占用的通道其字符设备
打开返回 `EBUSY`。

## 缓冲

| 队列 | 内存 | 方式 |
| --- | --- | --- |
| OUTPUT | ION（`onekvm_lazy_cma`，回退 carveout）物理连续，CPU 映射为写合并 | `MMAP`、`DMABUF` 导入/导出 |
| CAPTURE | vmalloc，驱动把 VENC 码流复制进来 | `MMAP` |

OUTPUT 由硬件原地读取，不复制。导入的 DMA-BUF 必须物理连续且在 4 GiB 以下。
CPU 逐像素写 OUTPUT 很慢（写合并、无缓存）；采集或缩放节点导出的缓冲应直接以
DMA-BUF 排入。CAPTURE 默认大小为 `宽 × 高 / 2`（H.26x 1–4 MiB，JPEG 512 KiB–4 MiB），
可在 `S_FMT` CAPTURE 的 `sizeimage` 指定 64 KiB–16 MiB；放不下的访问单元按单帧
失败处理。

## 控制

| 控制 | 范围 / 默认 | 生效 |
| --- | --- | --- |
| `MPEG_VIDEO_BITRATE` | 16 kbit/s–409.6 Mbit/s，默认 4 Mbit/s | H.264 下一帧（osdrv `495d985` 起，此前等下一个 GOP 起点）；H.265 下一个 AVBR 周期（≤ 20 帧） |
| `MPEG_VIDEO_BITRATE_MODE` | CBR / VBR（默认）；H.265 VBR 实为 AVBR | 会话开始 |
| `MPEG_VIDEO_FRAME_RC_ENABLE` | 关闭时为固定 QP（I/P QP 控制） | 会话开始 |
| `MPEG_VIDEO_GOP_SIZE` | 1–65535，默认 150 | 下一帧 |
| `MPEG_VIDEO_FORCE_KEY_FRAME` | — | 下一帧 |
| `MPEG_VIDEO_H264_PROFILE` | Baseline（CAVLC，默认）/ Main / High（CABAC） | 会话开始 |
| `MPEG_VIDEO_{H264,HEVC}_{MIN,MAX,I_FRAME,P_FRAME}_QP` | 0–51，默认 12 / 48 / 22 / 24 | 下一帧 |
| `JPEG_COMPRESSION_QUALITY`（仅 JPEG 节点） | 1–100，默认 85；100 按 99，50 按 51 | 下一帧 |

B 帧固定为 0，参数集固定随 IDR 输出。

## 统计

`/sys/class/video4linux/videoN/stats` 给出累计帧数、字节、错误、超时、跳帧、复位，
编码耗时（SendFrame 到取完码流）和延迟（OUTPUT `QBUF` 到完成）的平均、合计、
本次最大值及 0.5 ms 直方图。测试窗口的数值用前后差值计算。

## 107 实测（2026-11-11）

`v4l2-encode-smoke`（随模块包安装在 `/usr/libexec/onekvm/`）合成图案，1920×1080，
OUTPUT 3 缓冲、CAPTURE 4 缓冲，Core 停止时测：

| 场景 | 吞吐 | 延迟（QBUF→DQBUF，ms） |
| --- | --- | --- |
| H.264 NV12 不限速 | 103.7 fps | — |
| H.265 NV12 / UYVY 不限速 | 98.3 / 99.2 fps | — |
| JPEG NV12 / UYVY 不限速 | 224.6 / 171.5 fps | — |
| H.264 NV12 60 fps 实时 | 60.0 fps | 平均 9.56，P95 9.65，最大 13.80 |
| H.265 UYVY 60 fps 实时 | 60.0 fps | 平均 10.10，P95 10.13，最大 14.98 |

排空（`ENC_CMD_STOP`）在 H.264、H.265、JPEG 上均收到 `LAST` 与 `EOS`。

各格式输出均经主机 FFmpeg 完整解码，画面与输入图案一致；JPEG 输出带编码器写入的 2 字节空 APP9 段，FFmpeg 会对此告警。
Core 以 cvi_mpi 推 1080p60 时并发 V4L2 JPEG 1080p30 与 H.265 720p30，均 0 错误，
浏览器端仍约 60 fps。

## 源码

`osdrv-sg200x` 的 `interdrv/cvi_vc_drv/cvi_venc_v4l2.c`；共享的 ION vb2 内存层在
`interdrv/include/common/kapi/onekvm_vb2_ion.h`，供后续采集与缩放节点复用。
内核需启用 `V4L2_MEM2MEM_DEV` 与 `VIDEOBUF2_VMALLOC`（与解码器相同）。
