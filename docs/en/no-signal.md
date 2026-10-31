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

The checked-in files under `assets/no-signal/` and `src/no_signal_frames.inc`
are not used by the runtime.
