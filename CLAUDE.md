# onekvm-nanokvm-mmf 工作备忘

## 2026-08-18：测试机改分辨率后设备无画面

### 已确认现象

- 设备 `10.100.99.137`（NanoKVM Cube，`admin` / `no_password`）
- 被采集主机 `10.100.99.99`（HID 已连接，键盘灯 `known=true`，说明 USB gadget 正常）
- `GET /api/status`：`video.active=true`，`hdmi_connected=false`，`actual_fps=0`，`input_width/height=1920x1080`
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

### 未完成（环境限制）

当前 agent 不能 SSH 到 `root@10.100.99.137` / `root@10.100.99.99`，因此：

1. 无法在 99.99 上执行 `xrandr` 做分辨率切换复现
2. 无法把新的 `nanokvm-mmf.so` 部署到设备并只重启 `onekvm.service`
3. 无法读 `journalctl -u onekvm.service` 里的 `HDMI input changed to ...` 日志

允许 SSH 后的验证步骤：

```sh
# 设备上确认当前输入
ssh root@10.100.99.137 'onekvm-cli status video; cat /proc/cvitek/vi_dbg'

# 测试机切换一种已支持的模式，例如 1280x720
ssh root@10.100.99.99 'DISPLAY=:0 xrandr --output <HDMI> --mode 1280x720'

# 最多等约 3 秒，设备应重新出画
# journal 应出现：OneKVM: HDMI input resolution changed to 1280x720
# /api/status 应变为 hdmi_connected=true，actual_fps>0，input 1280x720
```

不要重启设备、不要动 RAUC/U-Boot；部署应用 so/IPK 后只重启 `onekvm.service`。
