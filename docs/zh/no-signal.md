# 无信号画面

[English](../en/no-signal.md) | 简体中文

`assets/no-signal/` 里的 PNG 是已批准的 Vue 画稿
（`onekvm-ui/src/components/NoSignal.vue`）在 1920×1080、1280×720、640×480
上的定稿截图。转成 packed NV21：

```sh
go run ./tools/pack_no_signal.go src/no_signal_frames.inc
```

运行时不解析 PNG。`soph-mmf.so` 通过 `render_no_signal_nv21` 展开这些数组。
绑定路径把 NV21 转成 UYVY 后 `CVI_VPSS_SendFrame`；WAVE4 仍走 VPSS→VENC 绑定
出 IDR/P。其它输出尺寸选用最近的 packed 资源。

`src/no_signal_h264.inc` 只给主机测试用，不在量产绑定路径上。改画稿只需重新生成
`src/no_signal_frames.inc`。
