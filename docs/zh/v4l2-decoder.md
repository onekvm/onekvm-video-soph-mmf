# SG2002 V4L2 解码接口

[English](../en/v4l2-decoder.md)

此接口接收压缩码流并输出原始帧。HDMI 采集、VPSS 缩放和硬件编码继续使用
MMF 流水线，不增加 V4L2 编码节点，也不修改 Core 的编码后端。

当前状态：实验实现，已在 107 的 6.18.45-onekvm-preempt 上完成基础实机验收。
它仍不是编码路径的替代品；节点注册和编译成功不能代替真实码流解码与资源释放验证。

## 接口范围

对外使用稳定别名 `/dev/onekvm-vdec`，不暴露厂商驱动名或动态的 `videoN` 编号。
模块加载脚本和 udev 规则都会维护这个别名；别名不可用时，启动时枚举 `/dev/video*`，
用 `VIDIOC_QUERYCAP` 查找驱动名以 `-vdec` 结尾、并提供
`V4L2_CAP_VIDEO_M2M_MPLANE | V4L2_CAP_STREAMING` 的节点。

| OUTPUT 压缩格式 | CAPTURE 原始格式 |
| --- | --- |
| H.264 Annex-B，完整访问单元，I/P 帧 | `V4L2_PIX_FMT_YUV420M` |
| 8-bit baseline JPEG/MJPEG | `YUV420M`、`YUV422M`、`YUV444M`、`GREY`，跟随码流 |

H.265 解码未开放。单包上限 2 MiB；可见画面范围 32×32 至 2880×1620。
实际可用分辨率和帧率需单独验收，格式枚举不是性能保证。

CAPTURE 的尺寸按 16 像素对齐；`VIDIOC_G_SELECTION(COMPOSE)` 返回可见区域。
JPEG 输出使用完整范围的 JPEG/BT.601 色彩元数据；H.264 当前默认 BT.709，
尚未从码流 VUI 传播色彩元数据。

支持 MMAP，以及 vb2-vmalloc 支持的 DMA-BUF 导入/导出。硬件参考帧保存在解码器
自己的 ION 缓冲里，输出按行复制到 vb2；此实现不提供解码零拷贝。

## 会话与缓冲

1. 设置 OUTPUT 格式，分配缓冲，按访问单元排队并启动 OUTPUT。
2. 订阅并等待 `V4L2_EVENT_SOURCE_CHANGE`，读取 CAPTURE 格式和可见区域。
3. 分配并排队 CAPTURE 的各个平面，启动 CAPTURE 后取解码帧。
4. 分辨率或 JPEG 采样变化时，先排空旧 CAPTURE 到 `LAST`，停止并释放其缓冲，
   再读取新格式并重建 CAPTURE。OUTPUT 会话继续保留。
5. `V4L2_DEC_CMD_STOP` 排空已排队的输入，最终返回空的 CAPTURE `LAST` 缓冲和
   `V4L2_EVENT_EOS`。排空期间拒绝新的 OUTPUT `QBUF`。
6. 排空后 `V4L2_DEC_CMD_START` 重置解码状态；重新输入须包含所需的参数集。
   关闭或停止 OUTPUT 会释放解码器、参考帧和会话占用。

当前只允许一个活动解码会话。SG2002 的 H.264/H.265 核共享 SRAM，H.264 解码
与 H.265 编码互斥：通过共享的会话创建门禁在初始化硬件前拒绝冲突。
这项门禁不修改编码参数、码流处理或 MMF 采集路径。

JPEG 先解析并核对头部、位深、尺寸和 SOI/EOI，再分配独立的非缓存 ION 输出。
`GetFrameData` 释放 JPU 事务锁后，输出缓冲继续保留到 CAPTURE 完成复制；
等待应用重建 CAPTURE 时不持有共享 JPU 锁。

## 源码与构建

Linux 的 `drivers/media/platform/sophgo/sophgo-vdec.c` 负责 V4L2、vb2、事件和队列。
`include/media/sophgo-vdec.h` 定义内核硬件操作接口；`osdrv-sg200x` 的
`interdrv/cvi_vc_drv/cvi_vdec_v4l2.c` 提供 H.264/JPEG 硬件实现。

`CONFIG_VIDEO_SOPHGO_VDEC=m` 选择必要的媒体模块。它们由
`onekvm-device-nanokvm-kernel-module-video-decoder` 打包；厂商模块包依赖该包。
调试构建只使用 `onekvm-distro` 的 OE/kas 工作树入口，不使用树外交叉编译。

模块包附带 `/usr/libexec/onekvm/v4l2-decode-smoke`，用公开 V4L2 ioctl 解码
测试码流。命令格式为 `v4l2-decode-smoke h264|mjpeg|jpeg packets.bin raw.yuv`
（可选保持秒数和重启次数）；工具优先使用 `/dev/onekvm-vdec`，并在别名缺失时自动选择
`*-vdec` 节点。输入文件重复排列
little-endian `uint32_t` 包长和压缩访问单元；输出是去除行及高度填充后的可见原始平面。
必须对比同一码流的软件解码参考帧，
并覆盖 EOS、关闭重开、分辨率变化、错误输入、争用和既有 WebRTC 画面。

107 已实测 H.264 640×480、640×360、1920×1080 分辨率切换共 30 帧，JPEG
420/标准 422/标准 444、灰度、采样切换以及截断/非法/超大输入；Core 恢复后
WebRTC 仍为 1920×1080 H.264 60 FPS。灰度与彩色之间的 CAPTURE 平面数变化
需要应用在收到事件后立即停止并重新分配队列，不能继续 QBUF 旧平面数的缓冲。
