# SG2002 V4L2 采集接口

[English](../en/v4l2-capture.md)

`soph_vi` 注册 V4L2 采集节点 `soph-vi`：MIPI-rx（CIF）经 VI YUV 旁路 DMA 写出
UYVY，即 LT6911 HDMI 桥片接入的通路。STREAMON 通过内核内的 SDK 层配置 CIF、VI
设备、管道和通道，不经过 cvi_mpi。桥片（时序、EDID、I2C）仍由用户态负责，在
STREAMON 前用 `S_FMT` 设置当前有效尺寸。

状态：实验实现，已在 107（6.18.57-onekvm-preempt）完成采集、起停、与编码节点
串接及与 cvi_mpi 互斥的验收；Core 尚未改用。

## 节点

| 节点名 | 类型 | 格式 | 尺寸 | 帧率 |
| --- | --- | --- | --- | --- |
| `soph-vi` | 单平面 `VIDEO_CAPTURE`，`STREAMING` | `UYVY` | 64×64 至 2880×2160，宽为偶数 | `S_PARM` 1–240，默认 60 |

`videoN` 编号动态分配（`soph-vi` 先于编解码节点加载），按节点名或
`VIDIOC_QUERYCAP` 的 `card` 查找。`bytesperline = ALIGN(宽 × 2, 16)`，
`sizeimage = bytesperline × 高`。

## 会话

1. `S_FMT` 设为桥片当前输出尺寸；尺寸与实际信号不符时 VI 报错或不出帧。
   流运行或缓冲已分配时 `S_FMT` 返回 `EBUSY`。
2. `S_PARM` 设置标称帧率（流运行时 `EBUSY`）。
3. `REQBUFS` 至少 3 个：VI 始终占用 2 个，用户态至少需要 1 个。
4. `STREAMON` 配置 MIPI-rx 与 VI 并开始采集，首帧约 20–35 ms 后到达。
5. `DQBUF` 的 `sequence` 来自 VI 帧计数（从 0 开始，缺号计入 `dropped`）；
   时间戳为 `MONOTONIC`，取 VI 帧结束时刻（`TSTAMP_SRC_EOF`）。
6. 尺寸变化需 `STREAMOFF` → `REQBUFS 0` → `S_FMT` → 重新分配并 `STREAMON`。
   节点不发事件；信号变化由用户态从桥片获取。

用户态持有缓冲期间 VI 不会写入；可用缓冲不足时 VI 等待（不覆盖），计入 `starved`。

## 与 cvi_mpi 互斥

VI 同一时刻只属于一方：`/dev/cvi-vi` 已打开时 `STREAMON` 返回 `EBUSY`；流运行
期间打开 `/dev/cvi-vi` 返回 `EBUSY`。节点的 VB 池为内核池：用户态 VB ioctl 不能
取块、释放块或销毁该池（`EPERM`），`VB_Exit` 也不会减掉内核引用。因此流运行期间
启动旧 Core 时，其残留 VB 回收失败、MMF 初始化报错，采集不受影响。

## 缓冲

| 方式 | 内存 | 说明 |
| --- | --- | --- |
| `MMAP` | ION（`onekvm_lazy_cma`，回退 carveout）物理连续 | 默认写合并、无缓存 |
| `MMAP` + `V4L2_MEMORY_FLAG_NON_COHERENT` | 同上，带缓存 | CPU 读快约 3 倍，`DQBUF` 时做缓存失效 |
| `EXPBUF` | 导出为 DMA-BUF | 直接排入 `soph-venc` / `soph-jpegenc` / `soph-vpss`（[v4l2-scaler.md](v4l2-scaler.md)）的 OUTPUT |

导出缓冲以 `V4L2_MEMORY_DMABUF` 排入编码节点时，`planes[0].length` 填缓冲大小，
`bytesused` 填 `sizeimage`。

## 统计

`/sys/class/video4linux/videoN/stats`：`frames`（已交付帧，累计）、`dropped`
（VI 帧号缺口，累计）、`starved`（本次流中 VI 无可用缓冲的次数）。

## 107 实测（2026-11-11）

99 输出 1920×1080@60，Core 停止，`v4l2-capture-smoke`（随模块包安装到
`/usr/libexec/onekvm/`）：

| 场景 | 结果 |
| --- | --- |
| 600 帧，4 缓冲 | 60.00 fps，间隔 16.66–16.68 ms，序号无缺口 |
| VI 帧结束 → `DQBUF` | 平均 0.15 ms，P95 0.16，最大 2.00 |
| 首帧（26 次起流） | `STREAMON` 后 19.8–34.4 ms |
| 20 次起停 | 20/20 |
| 3 缓冲，每帧占用 40 ms | 25.1 fps，VI 等待不覆盖，`STREAMOFF` 正常 |
| CPU 读 4 MB 帧 | 写合并 102 ms；`NON_COHERENT` 33 ms（`DQBUF` 平均 0.82 ms，P95 2.80） |
| → `soph-venc` H.265（DMA-BUF），600 帧 | 60 fps，0 错误；VI 帧结束 → 码流 `DQBUF` 平均 10.33 ms，P95 10.30，最大 28.06；FFmpeg 解出 600 帧 |
| → `soph-jpegenc`，120 帧 | 60 fps，0 错误；平均 6.18 ms，P95 8.12，最大 9.37；FFmpeg 解出 120 帧 |
| 流运行中启动旧 Core | 采集持续，用户态释放被拒 16 次，Core MMF 初始化失败；期间一次 1.74 s 停顿，同时 VI 报一次 `isp_err_chk` 并自动重触发（帧号连续，原因未查明）；Core 停止后采集正常 |
| 这些模块下的旧 Core（cvi_mpi） | 登录网页 1080p 60 FPS |

27 次起流中有 1 次首帧报 `isp_err_chk`（CSIBDG 宽或高超出设定），VI 自动重触发
后正常出帧。

## 源码

`osdrv-sg200x` 的 `interdrv/vi/chip/cv181x/vi_v4l2.c`；`vi.c` 提供与字符设备的
互斥（`vi_v4l2_acquire` / `vi_v4l2_release`）和 CIF 调用。内核需
`VIDEOBUF2_V4L2`，否则不编译该节点。
