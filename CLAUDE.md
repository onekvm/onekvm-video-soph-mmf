# onekvm-nanokvm-mmf 工作备忘

## 未完成 / 勿忘

- 看门狗已写但未部署到 `10.100.99.137`（需同时更新 `onekvm-core` + `onekvm-watchdog`）。
- NFS export 不含 `.137`（只有 103/107/196）；部署靠 scp。
- 设备 libgcc 15.2，OE 包依赖 15.3，安装需 `--force-depends`。
- 设备卡死时 `onekvm-server` 可能 SIGKILL 无效，只能 `reboot -f`。
- 0 FPS 可能只是显示器休眠，不是进程卡死；看门狗只认 D-Bus 心跳。
- 超范围 HDMI（1366x768 / 1440p / 4K）不得按旧几何继续采，应占位并报告实测尺寸。
- 活 VENC 通道上的 `SetChnAttr` / `RequestIDR` / `close_encoder` 只能在 reader 线程、两次 `GetStream` 之间做。HTTP/`g_mmf_mutex` 上调会和 `GetStream` 抢 `EnterVcodecLock`。

## 2026-08-19：目标 FPS 热改（r17 已上 137）

`0f14e22`：`set_h26x_output_fps` 只写 `pending_output_fps`；reader 在 `GetStream` 前 `apply_h26x_output_fps`（VBR src=60、dst=目标、gop=fps）再要 IDR。
真机 r17 + core r7：UI 60→30→60，PID 不变，`/proc/cvitek/venc` `TarFr`/`EncFramePerSec` 跟上。reader 的 printf 可能被 stdout 全缓冲，journal 看不到 `output fps` 不代表没生效。
审查：同 codec 热路径过宽会把 `Suspend` 变成空操作（reader 空转）。`5e98c87` / r18 已上 137：热路径只覆盖 fps/gop；`stop_h26x_reader` join 再 `DestroyChn`；`!packet_pending` 时 reader sleep。关会话后 VENC 通道消失，进程不空转。

## 2026-08-18：测试机改分辨率后设备无画面

### 已确认现象

- 设备 `10.100.99.137`（NanoKVM Cube，`admin` / `no_password`）
- 被采集主机 `10.100.99.99`（KDE Wayland / kwin，用户 `samlm` seat0）
- 测试机 HDMI-A-1 已改回 `1920x1080@60`（mode 8）。720p 热切换用 mode 23。
- HID 已连接，键盘灯 `known=true`，说明 USB gadget 正常
- `GET /api/status`：`video.active=true`，`hdmi_connected=false`，`actual_fps=0`，`input_width/height=1920x1080`（设备仍按旧的 1080p 几何采集）
- ATX `pwr_led=false`（该 GPIO 不能用来判断 99.99 是否开机）
- `/api/stream` MJPEG 8 秒无字节；设备日志没有 `HDMI input changed` / pipeline rebuilt

### 根因

默认 H.264 走 VPSS→VENC 绑定路径（`encoder_read_packet`），从不调用 `source_read`。
分辨率探测原先只写在 `source_read`，而且无信号时会清零失败计数并返回内置无信号帧，因此：

1. 源分辨率变化后 VI 仍按旧几何配置
2. VENC 不再出包，`actual_fps` 掉到 0
3. 绑定路径永远不会去读 LT6911 HDMI 时序
4. 管线停在旧的 1920x1080 上

### 已做修改（r15 已上机）

绑定空闲读包和 raw `source_read` 共用 `maybe_rebuild_for_hdmi_change()`。
`0e8fd87`：VENC 也停时占位图路径必须先 `failures++` 再 `maybe_rebuild`，否则永远不探 LT6911。
`261f1bb`：`recent_frames` 改看 VI IntCnt，避免活流 GetStream 卡死时误探 LT6911。
2026-08-19 r16 真机 1080↔720 双向约 1–3s 重建，画面恢复。

### 2026-08-18 部署记录

- 测试机 HDMI-A-1 已改回 `1920x1080@60`（`kscreen-doctor output.HDMI-A-1.mode.8`）。
- OE：`make -C onekvm-distro nanokvm-mmf` 产出
  `onekvm-device-nanokvm_0.1.0+git0+62f7cf0296-r3_onekvm_nanokvm.ipk`。
- `10.100.99.137` 不在 NFS export（只有 103/107/196），改为 scp +
  `opkg install --force-reinstall --force-depends`（设备 libgcc 仍是 15.2，
  配方依赖写成了 15.3）。
- 只重启了 `onekvm.service`。第一次起来：`OneKVM: configuring HDMI input 1920x1080`，
  但 `/proc/cvitek/vi_dbg` 仍是 `VIFPS: 0`，CSI 计数全 0。
- 第二次 `systemctl restart` 卡在 `deactivating (stop-sigkill)`，
  `onekvm-server` 杀不掉（媒体驱动卡死）。未授权前不做 NanoKVM 断电。

无信号画面根因：默认 H.264 绑定路径不走 `source_read()`，预设 NV21 从未进 VENC。
已在 `62f7cf0` 用 `encode_bound_placeholder()` 修。审查发现 Core 每轮
`BindVideoSource` 会在 `source_bound==false` 时重新绑 VPSS，占位提交被
`packet_pending` 吃掉，最多闪一帧。已改为占位期间 Bind 视为 no-op，
并无信号时把 `vi_dbg` 探测间隔收到 1s，方便同分辨率插回 HDMI。
