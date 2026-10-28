# OneKVM backend ABI

[English](../en/abi.md) | 简体中文

`soph-mmf.so` 只导出两个符号，见 `src/onekvm_backend.map`：

| 符号 | 头文件 | 用途 |
|------|--------|------|
| `onekvm_video_backend_query` | `include/onekvm/video_backend_v1.h` | 采集、编码、EDID、截图 |
| `onekvm_crypto_backend_query` | `include/onekvm/crypto_backend_v1.h` | 可选 SRTP TX AES-GCM |

这是 C ABI：结构体只能追加字段，调用方和后端都先看 `struct_size`。
厂商 MMF 符号全部 hidden，不进入 Core 的动态链接。

当前 `abi_version` 都是 `1`。请求不兼容版本时，query 失败，Core 在加载阶段拒绝该后端。

## 视频

`driver_id` 为 `soph-mmf`。编解码掩码：`auto`、H.264、H.265、MJPEG。

### 特性位

| 位 | 含义 |
|----|------|
| `SIGNAL_PRESENT` | `source_signal_present` |
| `KEYFRAME` | `encoder_request_keyframe` |
| `BORROWED_PACKET` | `encoder_release_packet` |
| `PREPARE_ENCODE` | `encoder_prepare`（手动路径的异步编码衔接） |
| `BOUND_ENCODER` | `encoder_bind_source` / `encoder_read_packet` / `encoder_unbind_source` |
| `LATENCY` | `source_latency` / `encoder_latency`，只读缓存 |
| `EDID` | `edid_capabilities` / `edid_get` / `edid_set`，不需要 source handle |
| `ENCODER_ALLOCATION` | `encoder_resources` / `encoder_allocate` / `encoder_allocation` |
| `SOURCE_SNAPSHOT` | `source_snapshot` |
| `BOUND_ENCODED_TIME` | 绑定 H.26x packet 的 `pts_ns` 为编码出包的 `CLOCK_MONOTONIC` 纳秒时间；0 未测得 |
| `ENCODER_COLOR_INFO` | `encoder_color_info` 返回实际编码分量位深和范围，只读缓存 |

未实现 `INTERRUPT_READ`（`source_interrupt` 为 `nullptr`）。status/SSE 路径不得 `GetFrame`、`GetStream` 或 dump procfs。

### 广告分辨率

ABI 表只列常用输出档，不是 HDMI 输入白名单：

```text
2880x1620@30  2560x1440@30  1920x1080@60  1280x720@60  640x480@60
```

像素格式为 NV21。输入接受规则和完整可选档见 [video-pipeline.md](video-pipeline.md) 与 `include/input_resolution_tracker.hpp`。

### 绑定与手动

KVM 主路是 **bound**：硬件把 VPSS 接到 VENC，Core 只 `encoder_read_packet`。
活路上不要 `source_read` 或 `encoder_encode`。

手动路径（`encoder_encode` / `source_read`）是内部回退：例如同时需要原始帧时，可能拷一帧 NV21。
这不是第二套对外接口。绑定活路上禁止 `SendFrame`。

### 编码器租约

`encoder_resources` 当前 policy：一路 H.26x + 一路 JPEG。
`hardware_channel_capacity` 是厂商通道数，调用方选择的是 policy 和 input shape，不是硬件通道号。

- `REALTIME`：KVM 主路。
- `BACKGROUND`：不自动抢占实时 encoder。KVM 空闲也不表示 encoder 已销毁。后台合成需要 Core 显式交接。

`encoder_create` 仍存在，供旧调用方使用；新的受管路径走 `encoder_allocate`。

### 截图

`source_snapshot` 复用已有 VPSS，申请 JPEG，把成品写进调用者缓冲区。

- `timeout_ms` 上限 1000；0 表示默认 250 ms。
- 显式宽高必须匹配当前输出，或都为 0。
- 已确认无信号 / `out_of_range` 返回 `UNSUPPORTED`，不把占位图编成 JPEG。
- 调用方不得为截图再开一个 MMF source。

### EDID

库级接口，不依赖 source handle。`edid_set` 成功后通过 `apply_required` 告诉宿主要不要热插拔或重启；库本身不 `reboot`。

- PCIe：可写 + 持久 + 热插拔（`apply_policy=HOTPLUG`）；永久写入仍走原 Flash writer。
- 已验证的 PCIe UXC 固件额外提供 `CAP_TEMPORARY` 与表尾 `edid_set_mode`。
  启动前核对真实硅片 ID `17 04 83`、Shadow/HPD/看门狗函数的固件字节特征，
  再验证 RAM 窗口。未知固件不开放临时写入。
- Cube C 另按真实 ID `16 05`、启动 HPD、RAM writer 和 EDID 来源选择的固件特征
  检测临时能力。`C090` 仅有已验证的顺序写入协议，不能把读值当作 RAM 内容。
  使用固件选中的有效永久／内置 EDID和 `/run` 中的上次已知值作为事务恢复基线；
  基线不明、恢复记录损坏或恢复失败时不开放临时能力。
- 临时请求固定 256 字节（扩展计数 1），成功返回 `HOTPLUG`；不写 Flash 和
  `/var/lib/onekvm/edid/active.bin`，也不调用 GPIO reset。RAM 在断电或芯片复位后丢失。
  写入前暂停该 source 的 MMF；同 source 锁和全局事务锁排除 watcher/encoder，
  恢复时沿用已有 generation 失效机制重新建流。
- C 的永久写入在 Shadow 能力通过验证时，保存 Flash 后显式写同一份 RAM EDID
  并触发 HPD，成功返回 `HOTPLUG`。没有该能力或保存成功后即时应用失败时返回
  `POWER_CYCLE`，表示已保存、等待激活，不能按写入失败自动重试 Flash。
  HPD 本身与重启 Linux 均不会重新加载 Flash。PCIe 永久路径保留 GPIO 复位。
  库不自行重启；UI 不得将 `POWER_CYCLE` 变成系统重启按钮。

## 加密

`backend_name` 为 `nanokvm CryptoDMA AES-GCM`，设备节点 `/dev/onekvm-crypto-offload`。

广告：`AES_GCM_TX`、`AES_GCM_TX_BATCH`。
**不**广告 `CONCURRENT_H265_VIDEO`：H.265 VENC 与 CryptoDMA 同时跑会锁死 SG2002，Core 对 H.265 会话走软件 AES-GCM。

完成位是 `CRYPTODMA_WR_INT` 轮询，不要等活 DTB 上从不触发的 PLIC 59。
单次 ioctl `ETIMEDOUT` 只让当前批次走软件。Core 使用 1、2、4、8、16、30 秒（封顶）
指数退避自动探测恢复，避免视频线程连续等待故障硬件，同时不在进程生命周期内永久禁用
offload。

### 编码流色彩信息

声明 `ENCODER_COLOR_INFO`（bit 14），在 API 表尾实现 `encoder_color_info`。
返回带 `struct_size` 的 `onekvm_video_color_info_v1`：位深为每个编码分量的位数；
范围区分未知（0）、有限（1）、全范围（2）和明确未标注（3）。
MMF 当前编码输入为 8-bit NV21；创建硬件通道后、绑定/StartRecvFrame 前只读 VUI，
缓存有效范围。H.264/H.265 缺少 video-signal 标记时，按规范将
`video_full_range_flag` 推定为 0，返回有限范围；VUI 查询失败仍返回未知。
回调仅读取原子缓存，不取帧、不读 I2C/proc、不等待 VENC；关闭/重建清理旧缓存。
旧 Core 忽略新增表尾，旧后端在新 Core 中显示未知，不按 codec 名称补猜。
