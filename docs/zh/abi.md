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

- PCIe：可写 + 热插拔（`apply_policy=HOTPLUG`）。
- Cube / Lite：可写 + 持久，`apply_policy=REBOOT`。库不在 `edid_set` 里重启。

## 加密

`backend_name` 为 `nanokvm CryptoDMA AES-GCM`，设备节点 `/dev/onekvm-crypto-offload`。

广告：`AES_GCM_TX`、`AES_GCM_TX_BATCH`。
**不**广告 `CONCURRENT_H265_VIDEO`：H.265 VENC 与 CryptoDMA 同时跑会锁死 SG2002，Core 对 H.265 会话走软件 AES-GCM。

完成位是 `CRYPTODMA_WR_INT` 轮询，不要等活 DTB 上从不触发的 PLIC 59。
第一次 ioctl `ETIMEDOUT` 后进程内禁用 offload，避免视频线程每帧空等 1 秒变成 1 FPS。
