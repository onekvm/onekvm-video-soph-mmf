# SG2002 动态多媒体内存（设计草案）

状态：设计阶段，尚未实现。本文记录目标、现状证据、方案及待验证项；实测结论落地后再并入
[视频流水线规范](video-pipeline.md)。

## 目标

- 视频驱动（VI/VPSS/VENC）与 NPU（TPU 运行库）使用的物理连续内存，平时对 Linux 可见、可用。
- 按需分配：视频或 NPU 需要时再取得连续区域；首帧允许因此增加 100–200 ms。
- 类似 ZFS ARC：调用方释放后先保留，下次直接复用；只有内存压力大、且该区域当前无人使用时，
  才归还给 Linux。正在被硬件使用的缓冲永不回收。

## 现状（107，NanoKVM PCIe，2026-11-05）

| 项 | 值 |
| --- | --- |
| 预留区 | `ion-region`（`soph_default_memmap.dtsi`），大小由 `CONFIG_ION_SIZE_MIB` 决定，107 为 64 MiB，位于 `0x85000000`；Linux 不可用 |
| 实际占用 | 40.1 MB，峰值 41.1 MB；约 24–26 MB 从未被分配 |
| 无视频消费者时 | 仍占 40 MB：`VbPool0`（VI，3×7.4 MB）、`VbPool1`（VPSS，9.3 MB）、VENC 参考帧 2×3 MB、码流 1 MB、H.264 固件 0.75 MB、ISP/VI 命令缓冲等 |
| 系统内存 | `MemTotal` 175 MB，`MemAvailable` 约 115 MB，zram 16 MB |
| 内核能力 | `CONFIG_CMA=y`、`CONFIG_ION_CMA_HEAP=y`；cvitek ION 已识别 `cvitek,cma_vpp`／`civtek,cma`（`ION_HEAP_TYPE_DMA`） |
| 唯一分配入口 | 用户态 `CVI_SYS_IonAlloc` → `SYS_ION_ALLOC` ioctl → `osdrv-sg200x/interdrv/sys/common/sys.c` 的 `_sys_ion_alloc()`，写死 `ION_HEAP_TYPE_CARVEOUT`；内核模块直接调用同一 `sys_ion_alloc()`。视频与 NPU 都经过这里 |

现有 ION CMA 堆每次分配都 `cma_alloc` 并清零整块，释放即 `cma_release`，没有保留缓存与压力回收，
不能直接满足目标。

## 方案

```
DDR ── CMA 区域（shared-dma-pool, reusable；计入 MemTotal / CmaTotal）
        │ 未分配页：Linux 仅放可迁移页面（页缓存、匿名页）
        ▼
  保留式 ION 堆（新）◄── sys.c ◄── VI/VPSS/VENC、TPU 运行库
    ├ 分配：保留缓存中按大小最佳匹配 → 无合适块时 cma_alloc（迁移页面）
    ├ 释放：块进入保留缓存，不立即 cma_release
    └ shrinker：count 只报告未被引用的保留块；scan 按 LRU 归还 CMA
```

| 机制 | 设计 |
| --- | --- |
| 区域 | 设备树 `ion-region` 改为 `compatible = "shared-dma-pool"; reusable;`，大小沿用 `CONFIG_ION_SIZE_MIB`。CMA 区不能承载不可迁移的内核分配，256 MB 机型不宜扩大 |
| 分配 | 先查保留缓存（同大小或最小可容纳块）；未命中则 `cma_alloc`。失败时先回收全部空闲保留块再重试一次，不回退到其他堆 |
| 清零 | 新取得的 CMA 页必须清零；复用保留块默认也清零，避免跨调用方泄漏。若同一调用方显式声明可复用旧内容，可跳过 |
| 保留 | 释放后的块记录大小、物理地址和最近释放时间；不保留仍被 dma-buf 引用的块 |
| 回收 | `register_shrinker`；`count_objects` 返回空闲保留页数（扣除最小保留量），`scan_objects` 按最久未用顺序 `cma_release` |
| 参数 | sysfs：保留上限、最小保留量（默认 0，内存紧张时可全部回收）、命中／未命中／迁移次数和耗时、回收页数 |
| 选择 | `sys.c` 优先请求新堆；仅在新堆不存在（旧设备树）时回退 carveout，便于同一 osdrv 兼容新旧内核 |

ZFS ARC 对应关系：区域本身是 Linux 可用内存；保留缓存相当于 ARC 中可驱逐的数据；`arc_min`／`arc_max`
对应最小保留量／保留上限；ARC 在内存压力下由 shrinker 收缩，本设计相同。

## 分阶段

| 阶段 | 内容 | 预期收益 | 主要风险 |
| --- | --- | --- | --- |
| 1 内核 | 保留式 ION 堆、设备树改为 reusable CMA、`sys.c` 换堆 | 视频运行时约 24 MB 未用区域回到 Linux；NPU 推理结束释放的缓冲可在压力下回收 | `cma_alloc` 遇到长期 pin 的页会失败；首次分配的迁移与清零耗时 |
| 2 MMF | 无消费者一段时间后释放 VI／VPSS VB、VENC 缓冲，交给保留缓存；有消费者时重新初始化 | 内存压力下空闲视频内存最多约 64 MB 可回收 | 见下节「VI DisableChn」；重新锁定 HDMI/CSI 可能超过 200 ms |

## VI DisableChn 为何不能恢复

[视频流水线规范](video-pipeline.md)记录：空闲调用 `CVI_VI_DisableChn` 后，再 `lt6911_start_csi` +
`SetChnAttr` + `EnableChn` 时 Preraw `IntCnt` 停在 0。驱动源码分析（`osdrv-sg200x/interdrv/vi/chip/cv181x`）：

| 步骤 | 驱动行为 |
| --- | --- |
| `vi_disable_chn()`（`vi_sdk_layer.c`） | 停流、销毁线程、`base_mod_jobs_exit`，并把 `total_chn_num` **和 `total_dev_num`** 都清零；随后 `vi_free_ion_buf()` 释放 ISP ION 缓冲 |
| `vi_set_chn_attr()` | `total_chn_num += devAttr.chn_num`，通道数恢复为 1 |
| `vi_enable_dev()` | 唯一递增 `total_dev_num` 的地方（并打开 MAC 时钟）；`VI_SDK_DISABLE_DEV` 是空操作 |
| `vi_start_streaming()` | `_vi_scene_ctrl()` 在 `is_ctrl_inited` 后直接取 `raw_max = total_dev_num`，`_vi_dma_setup(ctx, raw_max)` 等按设备数配置；设备数为 0 时 raw A 的 DMA 与前端不会被配置 |

MMF 的恢复序列只调用 `SetChnAttr` 与 `EnableChn`，没有再次 `EnableDev`，因此 `total_dev_num` 停在 0。

第二层：每轮出流的软件状态只在**首次打开 `/dev/vi`** 时由 `vi_open()` → `_vi_sw_init()` 重置，包括
`is_ctrl_inited`、`preraw_first_frm`、`isp_triggered`、`pre_be_frm_num`、`postraw_frame_number`、
`isp_pipe_enable` 等。`vi_disable_chn()`／`vi_stop_streaming()` 不重置它们，`vi_start_streaming()` 又因
`is_ctrl_inited` 提前返回场景配置。Core 重启或 MMF 完整 `DestroyVi`／`SYS_Exit` 再初始化（分辨率切换已实测可恢复）不受影响，推断与重新打开设备触发重置有关；
同一打开周期内直接 Disable→Enable 则带着旧状态启动。

### 107 实验（2026-11-05，临时 MMF 调试构建，未提交）

开流后立即 `CVI_VI_DisableChn(0,0)`，首个网页消费者到来时按 MMF 现有序列恢复；变体 2 在
`SetChnAttr` 前补 `CVI_VI_EnableDev(0)`。均为 1080p60 输入、H.264、登录网页取流约 20 s。

| 变体 | VI 通道 | `IntCnt`／`RecvPic` | 画面 | ION 占用 |
| --- | --- | --- | --- | --- |
| 1：`lt6911_start_csi` + `SetChnAttr` + `EnableChn` | `Enable=N` | 0／0 | 无信号占位 | 关闭期间 31.8 MB（常态 40.1 MB） |
| 2：再加 `EnableDev` | `Enable=Y` | 1／1 后停止 | 无信号占位 | 关闭期间 31.8 MB |

结论：设备计数归零是第一层原因，已由变体 2 证实（通道重新启用）；补齐后仍只收 1 帧，符合第二层
「未重置的每轮软件状态」推断，具体是哪一项状态阻止第二帧尚未定位。关闭 VI 可释放约 8 MB
ISP 缓冲，但 VI／VPSS VB 池仍由 VB 模块持有，须 `VB_Exit` 才能归还。

### 驱动修复（osdrv `8d646e2`）

| 改动 | 作用 |
| --- | --- |
| `total_dev_num` 由已启用设备标记计算，DisableChn 不再清零，最后一次 release 时清零 | 厂商用户态 `CVI_VI_EnableChn` 只有在设备数等于 1 时才下发 ioctl；之前清零后它直接返回成功，什么也不做 |
| 新一轮 EnableChn 前调用 `vi_reset_stream_state()` | 清 `isp_streamoff`、帧／触发计数和前后级状态；保留管线配置与已启用设备 |

107 验证（1080p60、H.264、WebRTC，临时实验开关在通道打开后立即 DisableChn，消费者到来时按
`lt6911_start_csi` + `SetChnAttr` + `EnableChn` 恢复，不调用 EnableDev）：

| 项 | 结果 |
| --- | --- |
| 不开实验开关的常规回归 | 60 FPS，`IntCnt` 持续增长，ION 40.1 MB |
| 同一进程 DisableChn→EnableChn | 4 轮（不重启 Core）均 60 FPS，`IntCnt` 持续增长，无占位 |
| 重新启用到首个 WebRTC 关键帧 | 约 140、57、58、50 ms |
| VI 关闭期间 ION | 打开后即关闭时 31.8 MB（ISP 缓冲已释放）；VI／VPSS VB 池仍在 |

可行路线：

| 路线 | 做法 | 首帧代价 | 状态 |
| --- | --- | --- | --- |
| A | 完整重开（`DestroyVi` + `SYS_Exit`，再 `initialize()`），可释放全部 VB | 约 0.7–1.5 s（含 LT6911 序列） | 已可用，超出预算 |
| B | 空闲时 VI DisableChn，消费者到来时同进程重新启用 | 50–140 ms（上表） | 驱动已修复；MMF 空闲策略未改，需判断模块版本 |

VB 池（约 31 MB）仍需 `VB_Exit` 或按池销毁才能交给保留式堆，这是阶段 2 的剩余工作。

## 待验证

- VI DisableChn 恢复在不同分辨率、断信号恢复及截图并发下的表现；VB 池能否在 VI 关闭期间按池销毁并重建。
- 阶段 1 前后 `MemAvailable`／`CmaFree`：空闲、视频中、NPU 推理后。
- 首帧时间：命中保留缓存与需要 `cma_alloc` 两种情况各至少三轮。
- 内存压力下 shrinker 实际回收，回收后视频／NPU 仍能分配且无 OOM。
- 1080p60／90、分辨率切换、OCR 与视频并发。

内核改动正式发布需递增系统版本 `y`。
