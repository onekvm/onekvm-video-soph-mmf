# V4L2 video backend (soph-v4l2.so)

[简体中文](../zh/v4l2-backend.md)

`soph-v4l2.so` implements the `onekvm::mmf` interface in `src/mmf.hpp` on the
kernel V4L2 nodes. The upper layers (ABI, encoders, snapshots, LT6911, EDID,
no-signal frames) are the same sources as in `soph-mmf.so`. It links only
libc, libstdc++ and libgcc, does not load cvi_mpi, and does not need
`onekvm-video-soph-mmf-runtime`.

Status: experimental. The device descriptor loads `soph-mmf.so` by default.

## Nodes and dependencies

| Use | Node | Document |
| --- | --- | --- |
| HDMI capture, NV21 straight from VI | `soph-vi` | [v4l2-capture.md](v4l2-capture.md) |
| Scaling (output size differs from HDMI) | `soph-vpss` | [v4l2-scaler.md](v4l2-scaler.md) |
| H.264 / H.265 | `soph-venc` | [v4l2-encoder.md](v4l2-encoder.md) |
| JPEG (MJPEG, snapshots) | `soph-jpegenc` | [v4l2-encoder.md](v4l2-encoder.md) |

Install it together with the `soph_*` modules that register these nodes. Older
modules lack them, and backend initialization fails. User space still drives
the LT6911 over i2c-dev (`src/lt6911_bridge.c`).

## Selecting the backend

The same recipe builds both backends into `/usr/lib/onekvm/video-backends/`.
Core loads the library named by `library` in
`/usr/share/onekvm/device-backends/nanokvm.json`:

```json
"library": "/usr/lib/onekvm/video-backends/soph-v4l2.so"
```

Restart `onekvm.service` after changing it. The two backends cannot run at the
same time: whichever opens VI first owns it.

CMake options: `ONEKVM_BUILD_V4L2_BACKEND` (default ON) builds
`soph-v4l2.so`; `ONEKVM_BUILD_MMF_BACKEND` (default ON) builds `soph-mmf.so`.
Only the latter needs `CVI_MPI_ROOT`.

## Data path

```text
LT6911 ─CSI─▶ soph-vi (NV21, MMAP, exported as DMA-BUF)
                 ├─ same-size channel ─────────▶ soph-venc / soph-jpegenc (OUTPUT DMABUF)
                 └─ scaled channel ─▶ soph-vpss ─▶ soph-venc / soph-jpegenc
```

- Threads: the control thread (upper-layer calls, serialized by
  `MmfControlLock`); one pipeline thread that routes VI frames to the capture
  channels, bound encoders and the latest-frame slot; one reader thread per
  H.26x encoder, the only one that dequeues its bitstream.
- Frames move between nodes as DMA-BUFs without CPU copies. Each encoder holds
  at most two queued frames. A frame that arrives while the encoder is still
  busy is dropped and counted rather than queued, so it adds no latency.
- The VI frame-end timestamp is carried through `soph-vpss` to the encoder
  output. The "Capture" item of the latency panel is the frame period plus
  (encoder dequeue time − VI frame end).
- Without a signal, the pipeline feeds placeholder frames from staging buffers
  at the VI size; the encoder configuration does not change.

| Buffers | Count | Lifetime |
| --- | --- | --- |
| VI NV21 | 6 (4 when a frame exceeds 4 MiB) | Only while VI streams; freed on pause |
| VPSS output (scaled channel) | 4 (3 when a frame exceeds 4 MiB) | Only while the channel runs |
| H.26x bitstream | 4 | While the encoder is open |
| JPEG bitstream | 2 | While the JPEG encoder is open |

## Encoder parameters

H.264 and H.265 rate control use the values of the cvi_mpi runtime:

| Item | Value |
| --- | --- |
| Rate control | H.264 VBR; H.265 AVBR |
| VBR/AVBR details | ChangePos 90, I/P ratio 1–100, MaxIQp = MaxQp − 2, IPQpDelta 2 |
| Default QP | min 12, max 48, initial 22 |
| Runtime changes | The reader thread applies frame rate/GOP, bitrate and QP between frames; IDR uses `FORCE_KEY_FRAME` |
| Frame skip | `FRAME_SKIP_MODE` Buffer limit is on whenever a bitrate cap is set (manual bitrate or the cap from automatic bitrate) |

On static content `soph-venc` emits skip blocks (P frames of about a dozen
bytes) where the cvi_mpi runtime emits about 9.6 KB per P frame. Both decode to
the same picture.

JPEG encodes borrowed capture frames directly (the same DMA-BUF). Only CPU
memory passed in by the caller is copied into a staging buffer.

## Idle and memory

When VI pauses:

1. After VI `STREAMOFF` every VI buffer is freed; scaled channels close their
   `soph-vpss` context.
2. Idle encoders (bound and no longer fed, or unbound with no packet pending)
   stop their OUTPUT queue and call `REQBUFS(0)`, which drops the imported
   DMA-BUFs. Otherwise vb2 keeps the last imported buffer attached and the
   "freed" VI memory never returns.
3. Stopping OUTPUT ends the VENC session. The next input streams OUTPUT on
   again and the new session starts with an IDR. A reconnect requests an IDR
   anyway, so the bitstream semantics do not change.

With MJPEG as the primary codec, the upper layers unbind the primary JPEG
encoder's source when the last MJPEG client disconnects, and the backend
then pauses VI and the scaling channel. That call used to do nothing: both
backends kept their capture buffers until H.26x was used again or the
service restarted. Unbinding a JPEG encoder that VNC bound logically leaves
capture running.

## Bitrate cap

H.264 uses VBR and applies a changed maximum bitrate from the next frame. H.265 uses AVBR and updates its model each statistics period. When the encoder reaches its QP limit, frame skipping can limit the output bitrate at the cost of frame rate.
