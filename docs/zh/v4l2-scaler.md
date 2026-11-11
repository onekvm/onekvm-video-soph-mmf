# SG2002 V4L2 缩放与格式转换接口

[English](../en/v4l2-scaler.md)

`soph_vpss` 注册内存到内存节点 `soph-vpss`：用 VPSS 硬件对一帧做裁剪、缩放、
翻转和格式转换。会话通过内核内的 VPSS 层建立分组，不经过 cvi_mpi。`soph-vi` 可
直接输出 NV21/NV12（见 [v4l2-capture.md](v4l2-capture.md#420-输出)），同尺寸编码
不需要此节点；缩放、裁剪、翻转或其他格式才经过它。

状态：实验实现，已在 107（6.18.57-onekvm-preempt）完成格式、尺寸、裁剪、翻转、
与采集和编码节点串接，以及与旧 Core 并存的验收；Core 尚未改用。

## 节点

| 节点名 | 类型 | 格式（OUTPUT 与 CAPTURE 相同） | 尺寸 |
| --- | --- | --- | --- |
| `soph-vpss` | `VIDEO_M2M_MPLANE`，`STREAMING`，单平面缓冲 | `NV12` `NV21` `NV16` `NV61` `YU12` `UYVY` `YUYV` `VYUY` `YVYU` `GREY` `RGB3` `BGR3` | 32×32 至 2880×4096 |

默认 OUTPUT 为 UYVY 1920×1080，CAPTURE 为 NV12 1920×1080。`videoN` 编号动态
分配，按节点名或 `VIDIOC_QUERYCAP` 的 `card`（`SG200x VPSS scaler`）查找。

- 多平面格式的各平面依次放在同一缓冲中；`num_planes` 为 1。
- 宽高按格式的色度采样取偶数（`GREY`、RGB 不限）。
- 缩小倍数须小于 32：`S_SELECTION` 会自动调整矩形；用 `S_FMT` 设出 1/32 及以下
  的比例时 `STREAMON` 返回 `EINVAL`。放大不受此限。
- CAPTURE 宽度大于 1920 时使用 `sc_v1` 通道，否则使用 `sc_d`；两者都在 VPSS
  `img_v` 上，与旧 Core 的分组轮流使用硬件。

## 行距与缓冲

| 队列 | `bytesperline` |
| --- | --- |
| OUTPUT，打包 YUV（`UYVY` 等） | 至少 `ALIGN(宽 × 2, 16)`，与 `soph-vi` 一致；可填更宽的值（不超过 2 倍，16 字节对齐） |
| OUTPUT，其他格式 | 至少按 64 字节对齐的最小行距；可填更宽的值（不超过 2 倍） |
| CAPTURE | 由节点决定，按 64 字节对齐（例如 NV12 1280 宽为 1280，RGB3 1280 宽为 3840） |

缓冲支持 `MMAP` 与 `DMABUF`。以 `DMABUF` 排入时 `planes[0].length` 填缓冲大小，
`bytesused` 至少为 `sizeimage`，`data_offset` 须为 64 的倍数。`MMAP` 可加
`V4L2_MEMORY_FLAG_NON_COHERENT` 得到带缓存的映射。

## 会话

1. 先 `S_FMT` OUTPUT，再 `S_FMT` CAPTURE；缓冲已分配时返回 `EBUSY`。`S_FMT`
   会把对应队列的裁剪或合成矩形重置为整幅。
2. 可选：
   - `S_SELECTION`：OUTPUT 设 `V4L2_SEL_TGT_CROP` 取源图区域，CAPTURE 设
     `V4L2_SEL_TGT_COMPOSE` 指定输出中的画面位置，其余部分填黑。
   - `V4L2_CID_HFLIP` / `V4L2_CID_VFLIP` 控制翻转。
   这些设置在流运行中也可修改，从下一帧开始生效。
3. 两个队列都 `STREAMON` 后节点建立 VPSS 分组；任一队列 `STREAMOFF` 即销毁。
4. 每个 OUTPUT 缓冲产生一个 CAPTURE 缓冲：
   - 时间戳、`field` 和时间码从 OUTPUT 复制到 CAPTURE，`soph-vi` 的 VI 帧结束
     时间可一直传到编码输出。
   - 处理失败的帧以 `V4L2_BUF_FLAG_ERROR` 返回。
   - 一帧 1 秒内未完成时节点放弃该帧并重建分组，后续帧继续处理。

## 色彩

- YUV 输入默认 BT.709 有限范围，RGB 默认 sRGB 全范围；`S_FMT` 填写的色彩参数
  原样保留。
- YUV 与 RGB 互转固定使用 BT.601 系数，不改变取值范围：
  - 有限范围 YUV 转出的 RGB 为 16–235；
  - 全范围 RGB 转出的 YUV 为 0–255。
- CAPTURE 的 `quantization` 与 OUTPUT 相同，`ycbcr_enc` 按此报告。

## 与 cvi_mpi 并存

- 节点的分组从最高号（15）往下取，旧 Core 使用的低号分组不受影响。
- 字符设备 `/dev/cvi-vpss` 对这些分组的 ioctl 返回 `EPERM`，因此旧 Core 回收
  残留分组时会跳过它们。
- 两者可同时运行，VPSS 硬件按帧轮流处理。

## 统计

`/sys/class/video4linux/videoN/stats`：

| 字段 | 含义 | 范围 |
| --- | --- | --- |
| `frames` | 完成帧数 | 累计 |
| `errors` | 失败帧数 | 累计 |
| `timeouts` | 超时帧数 | 累计 |
| `process_us_*` | 硬件处理时间：平均、合计 | 累计 |
| `process_us_max` | 硬件处理时间最大值 | 本次流 |
| `latency_us_*` | OUTPUT `QBUF` 到 CAPTURE 完成：平均、合计 | 累计 |
| `latency_us_max` | 上述时间最大值 | 本次流 |

## 107 实测（2026-11-11）

`v4l2-scale-smoke`（随模块包安装到 `/usr/libexec/onekvm/`）。

### 合成色条

`-t`：Core 停止，MMAP 输入为 1920×1080 色条，60 帧。

| 场景 | 结果 |
| --- | --- |
| UYVY → 12 种输出格式（720p） | 全部通过；YUV 输出误差 0，RGB 输出误差 ≤ 1（按上述色彩规则计算） |
| NV12、RGB3、YUYV 输入 | 色条误差 0 |
| 每帧硬件处理，1080p → 1080p/720p NV12 | 约 6.2 ms，约 150 fps |
| 1080p → 2560×1440 / 2880×1620 | 93 / 75 fps |
| 1366×768 → 720p | 270 fps |
| 32×32 → 1024×1024、1080p → 62×36 | 正常 |
| 1080p → 60×34（1/32） | 硬件不出帧，每帧 1 秒超时后重建分组，后续正常；现已由节点拒绝 |
| 水平翻转、裁剪、合成 | 正常 |

### 采集与编码串接

99 输出 1920×1080@60，Core 停止。延迟从 VI 帧结束起算。

| 场景 | 结果 |
| --- | --- |
| `soph-vi` → `soph-vpss` NV12 1080p，600 帧 | 60.00 fps，VI 序号无缺口；到缩放帧 `DQBUF` 平均 6.66 ms，P95 6.79，最大 6.86 |
| → NV12 720p | 60 fps；平均 6.65 ms，P95 6.79，最大 6.83 |
| → NV12 1080p → `soph-venc` H.264，4 轮各 600 帧 | 60 fps（首轮 59.4）；到码流 `DQBUF` 平均 16.4–17.4 ms，P50 16.4，P95 16.5–22.8；最大 23–157 |
| → NV12 720p → H.264 | 60 fps；平均 11.62 ms，P95 11.63，最大 31.23 |
| → NV12 1080p → H.265 | 60 fps；平均 17.42 ms，P95 17.51，最大 27.28 |
| 对照：UYVY 直送 H.265（不经 VPSS，同轮） | 60 fps；平均 10.28 ms，P95 10.30，最大 19.71 |
| → NV12 720p → JPEG，120 帧 | 60 fps；平均 9.87 ms，P95 10.03 |
| 中心 960×540 裁剪 → 1080p H.264 | 60 fps；平均 19.06 ms，P95 19.11 |
| 码流解码 | FFmpeg 解出全部帧，无错误；缩放与裁剪画面正确 |
| 10 次起停 | 10/10 |
| 合成流运行 40 秒期间启动旧 Core | 合成流 0 错误、0 超时，平均延迟 24.5 → 26.8 ms；Core 正常启动 |
| 这些模块下的旧 Core | 登录网页 H.264 1080p 60 FPS |

- 经 VPSS 比 UYVY 直送 H.265 多约 7 ms，即 VPSS 处理整帧的时间。该时间只取决于
  像素数：`soph-vi` 改为输出 NV21 时，到 1080p / 720p 缩放帧平均仍为 6.68 /
  6.67 ms，720p H.264 P50 同为 11.53 ms。旧路径记录的
  VPSS `CostTime` 约 6.1 ms（见 [video-pipeline.md](video-pipeline.md)），
  与此同量级。该值来自旧文档，不是同轮实测。
- 合成测试一次排入 4 帧，其 `QBUF` → `DQBUF` 时间含排队，应以
  `process_us` 衡量处理时间。

## 源码

`osdrv-sg200x` 的 `interdrv/vpss/common/vpss_v4l2.c`；`vpss.c` 提供内核分组
（`vpss_create_kernel_grp`）和完成回调，`vpss_ioctl.c` 拒绝字符设备访问这些
分组。内核需 `V4L2_MEM2MEM_DEV`，否则不编译该节点。
