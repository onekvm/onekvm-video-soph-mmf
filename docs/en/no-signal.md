# No-signal artwork

English | [简体中文](../zh/no-signal.md)

The PNGs under `assets/no-signal/` are deterministic screenshots of the
approved Vue artwork in `onekvm-ui/src/components/NoSignal.vue`, captured at
1920×1080, 1280×720 and 640×480. Convert them to packed NV21 with:

```sh
go run ./tools/pack_no_signal.go src/no_signal_frames.inc
```

The runtime does not parse PNG files. `soph-mmf.so` expands the generated
arrays through `render_no_signal_nv21`. The bound path converts that NV21 to
UYVY and `CVI_VPSS_SendFrame`s it; WAVE4 still emits IDR/P on the VPSS→VENC
bind. Other output sizes pick the nearest packed asset.

`src/no_signal_h264.inc` is host-test leftover and is not on the production
bind path. Changing the artwork only requires regenerating
`src/no_signal_frames.inc`.
