# No-signal artwork

English | [简体中文](../zh/no-signal.md)

`onekvm-ui` builds the PNGs into each language pack at
`/usr/share/onekvm/l10n/<lang>/no-signal/`. `onekvm-no-signal-cache` turns
those PNGs into NV21 under `/data/onekvm/cache/no-signal/<lang>/` and points
`current` at the active directory. A custom image at
`/data/onekvm/personalization/no-signal.png` is letterboxed into `custom/`
and takes `current` until it is removed.

`soph-mmf.so` reads `/data/onekvm/cache/no-signal/current/<width>x<height>.nv21`.
When that file is missing, the frame is black. Other output sizes use the
nearest cached picture and letterbox it. The backend re-reads the file when
the `current` target changes and starts the next picture with an IDR.

A new picture can overwrite the same NV21 allocation. When the cache identity
changes, invalidate both prepared VPSS DMA copies and convert and flush each
again before reuse. Comparing only the source pointer would keep showing the old picture.

The artwork has no capture clock, so it is submitted at the encoder's output
frame rate (60 fps by default). soph-v4l2.so used to submit a copy on every
call, as soon as the encoder returned the previous one. 1080p H.264 without
a signal on device 107, 2 rounds each:

| soph-v4l2.so | Submission | VENC rate | Bitrate | Core CPU |
| --- | --- | --- | --- | --- |
| Automatic bitrate | unpaced → output rate | 102–103 → 60 fps | 8.1–8.2 → 4.9–5.0 Mbps | 32% → 19–20% |
| 500 kbps cap (frame skip on) | unpaced → output rate | 314–320 → 59–60 fps | 2.13–2.15 → 0.31–0.34 Mbps | 47% → 16% |

Rate control budgets bits per frame at the output frame rate, so faster
submission breaks a bitrate cap. soph-mmf.so is paced by the vendor binding
(about 55 fps) and is unchanged.

The checked-in files under `assets/no-signal/` and `src/no_signal_frames.inc`
are not used by the runtime.
