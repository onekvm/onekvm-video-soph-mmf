# onekvm-nanokvm-mmf 工作备忘

## 2026-08-18：测试机改分辨率后设备无画面

### 已确认现象

- 设备 `10.100.99.137`（NanoKVM Cube，`admin` / `no_password`）
- 被采集主机 `10.100.99.99`（KDE Wayland / kwin，用户 `samlm` seat0）
- **测试机 HDMI-A-1 当前是 `800x600@60`，不是 1080p。** 该口首选模式仍是 `1920x1080@60`；内置 eDP-1 已 disable。
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

### 已做修改（未上机）

绑定空闲读包和 raw `source_read` 共用 `maybe_rebuild_for_hdmi_change()`：
出帧停止超过 500ms、连续失败 ≥ 3、距上次探测 ≥ 2s 后才读 LT6911；
两次稳定采样且分辨率变化才 `reopen` 整个 MMF 管线。

Host 单测：`g++ -std=c++17 -I include tests/input-resolution-tracker-test.cpp` 已通过。

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
