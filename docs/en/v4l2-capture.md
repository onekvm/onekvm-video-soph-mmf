# SG2002 V4L2 capture

[简体中文](../zh/v4l2-capture.md)

`soph_vi` registers the V4L2 capture node `soph-vi`: MIPI-rx (CIF) through the
VI YUV bypass DMA, writing UYVY, or NV21/NV12 converted by the CSI bridge while
writing. This is the path the LT6911 HDMI bridge feeds.
`STREAMON` programs CIF and the VI device, pipe and channel through the
in-kernel SDK layer, without cvi_mpi. The bridge (timing, EDID, I2C) stays in
user space, which sets the current active size with `S_FMT` before `STREAMON`.

Status: experimental, validated on device 107 (6.18.57-onekvm-preempt) for
capture, start/stop, chaining into the encoder nodes and exclusion against
cvi_mpi. The `soph-v4l2.so` backend uses this node; see
[v4l2-backend.md](v4l2-backend.md).

## Node

| Node name | Type | Format | Size | Frame rate |
| --- | --- | --- | --- | --- |
| `soph-vi` | single-plane `VIDEO_CAPTURE`, `STREAMING` | `UYVY` `NV21` `NV12` | 64×64 to 2880×2160, even width; NV21/NV12 also even height | `S_PARM` 1–240, default 60 |

`videoN` numbers are dynamic (`soph-vi` loads before the codec nodes); find the
node by name or by the `card` from `VIDIOC_QUERYCAP`.

| Format | `bytesperline` | `sizeimage` |
| --- | --- | --- |
| `UYVY` | `ALIGN(width × 2, 16)` | `bytesperline × height` |
| `NV21` / `NV12` | `ALIGN(width, 64)`; the chroma plane follows the luma plane | `bytesperline × height × 3 / 2` |

## 4:2:0 output

For `NV21` / `NV12` the CSI bridge converts 4:2:2 to 4:2:0 while writing to
memory. Luma is byte-identical to `UYVY`; chroma is the rounded average of each
pair of lines. VPSS is not involved, so:

- same-size H.264 can queue `soph-vi` buffers straight to `soph-venc`, saving
  the VPSS whole-frame time (about 6.7 ms at 1080p);
- H.265 and JPEG can also take `UYVY` directly; JPEG from `NV21` input is 4:2:0.

Scaling, cropping, flipping and other formats still need
[`soph-vpss`](v4l2-scaler.md).

## Session

1. `S_FMT` to the size the bridge currently outputs; a size that does not match
   the signal makes VI report errors or deliver no frames. `S_FMT` returns
   `EBUSY` while streaming or with buffers allocated.
2. `S_PARM` sets the nominal frame rate (`EBUSY` while streaming).
3. `REQBUFS` at least 3: VI always holds 2, user space needs at least 1.
4. `STREAMON` programs MIPI-rx and VI and starts capture; the first frame
   arrives after about 20–35 ms.
5. `DQBUF` `sequence` follows the VI frame counter (starting at 0; gaps count
   as `dropped`). Timestamps are `MONOTONIC`, taken at VI frame end
   (`TSTAMP_SRC_EOF`).
6. A size change needs `STREAMOFF` → `REQBUFS 0` → `S_FMT` → new buffers and
   `STREAMON`. The node raises no events; user space learns about signal changes
   from the bridge.

VI never writes into a buffer user space holds; when no buffer is free, VI waits
(it does not overwrite) and counts it as `starved`.

## Exclusion against cvi_mpi

VI belongs to one side at a time: with `/dev/cvi-vi` open, `STREAMON` returns
`EBUSY`; while streaming, opening `/dev/cvi-vi` returns `EBUSY`. The node's VB
pool is a kernel pool: user-space VB ioctls cannot get, release or destroy its
blocks (`EPERM`), and `VB_Exit` does not drop the kernel reference. Starting the
legacy Core during a stream therefore fails its stale VB reclaim and MMF init,
and capture continues.

## Buffers

| Mode | Memory | Notes |
| --- | --- | --- |
| `MMAP` | Physically contiguous ION (`onekvm_lazy_cma`, carveout fallback) | write-combined, uncached by default |
| `MMAP` + `V4L2_MEMORY_FLAG_NON_COHERENT` | same, cached | CPU reads about 3× faster; `DQBUF` invalidates the cache |
| `EXPBUF` | exported as DMA-BUF | queue directly to the `soph-venc` / `soph-jpegenc` / `soph-vpss` ([v4l2-scaler.md](v4l2-scaler.md)) OUTPUT with the same format and stride |

When queuing an exported buffer to an encoder as `V4L2_MEMORY_DMABUF`, set
`planes[0].length` to the buffer size and `bytesused` to `sizeimage`.

## Statistics

`/sys/class/video4linux/videoN/stats`: `frames` (delivered, cumulative),
`dropped` (VI frame number gaps, cumulative) and `starved` (times VI found no
free buffer in the current stream).

## Device 107 measurements (2026-11-11)

Host 99 outputs 1920×1080@60, Core stopped, `v4l2-capture-smoke` (installed with
the module package in `/usr/libexec/onekvm/`):

| Case | Result |
| --- | --- |
| 600 frames, 4 buffers | 60.00 fps, interval 16.66–16.68 ms, no sequence gaps |
| VI frame end → `DQBUF` | avg 0.15 ms, P95 0.16, max 2.00 |
| First frame (26 starts) | 19.8–34.4 ms after `STREAMON` |
| 20 start/stop cycles | 20/20 |
| 3 buffers, each held 40 ms | 25.1 fps, VI waits without overwriting, `STREAMOFF` normal |
| CPU read of a 4 MB frame | write-combined 102 ms; `NON_COHERENT` 33 ms (`DQBUF` avg 0.82 ms, P95 2.80) |
| → `soph-venc` H.265 (DMA-BUF), 600 frames | 60 fps, 0 errors; VI frame end → bitstream `DQBUF` avg 10.33 ms, P95 10.30, max 28.06; FFmpeg decodes 600 frames |
| → `soph-jpegenc`, 120 frames | 60 fps, 0 errors; avg 6.18 ms, P95 8.12, max 9.37; FFmpeg decodes 120 frames |
| Legacy Core started during a stream | capture continues; 16 user-space releases refused, Core MMF init fails; one 1.74 s pause with one `isp_err_chk` that VI retriggered (frame numbers continuous, cause not identified); capture normal after Core stops |
| Legacy Core (cvi_mpi) on these modules | logged-in web page 1080p 60 FPS |

One of 27 starts reported `isp_err_chk` (CSIBDG width or height above the
setting) on the first frame; VI retriggered and delivered frames normally.

### 4:2:0 output and latency comparison

Host 99 outputs 1920×1080@60, Core stopped. Latency runs from VI frame end to
the bitstream `DQBUF`. Two rounds in one session, 600 frames each (JPEG 300);
values are "round 1 / round 2".

| Path | P50 (ms) | P95 (ms) | Avg (ms) |
| --- | --- | --- | --- |
| `UYVY` → `soph-vpss` NV12 → H.264 | 16.46 / 16.48 | 17.47 / 17.38 | 16.75 / 16.65 |
| `NV21` → H.264 | 9.78 / 9.79 | 9.85 / 10.75 | 10.21 / 9.86 |
| `NV12` → H.264 | 9.79 / 9.78 | 9.92 / 9.93 | 10.66 / 9.85 |
| `UYVY` → `soph-vpss` NV12 → H.265 | 17.44 / 17.43 | 17.69 / 17.48 | 17.56 / 17.33 |
| `UYVY` → H.265 | 10.25 / 10.26 | 10.30 / 10.33 | 10.26 / 10.32 |
| `NV21` → H.265 | 10.25 / 10.25 | 10.30 / 10.31 | 10.27 / 10.28 |
| `UYVY` → JPEG | 5.90 / 5.91 | 8.39 / 8.26 | 10.96 / 6.42 |
| `NV21` → JPEG | 4.66 / 4.66 | 9.52 / 7.08 | 11.37 / 5.23 |

- Dropping VPSS from same-size H.264 lowers P50 by about 6.7 ms, the VPSS
  whole-frame time.
- In round 1, `NV21` and `NV12` → H.264 each had one stall (max 101 / 140 ms,
  59.6 / 59.4 fps), and both JPEG cases stalled 350–450 ms (UYVY and NV21
  alike). Round 2 had no stalls and ran at 60 fps. The cause was not found.
- Bitstreams: FFmpeg decodes every frame. PSNR of H.264 frame 300 against the
  UYVY reference: 48.6 dB through VPSS, 48.4 dB with `NV21` direct.

| Case | Result |
| --- | --- |
| 1920×1080 `NV21` / `NV12` against a `UYVY` frame captured alongside | luma error 0; chroma within 1 of the two-line average |
| Host 99 temporarily at 1368×768 (stride 1408) | same; H.264, H.265 and JPEG direct all decode correctly |
| 30 alternating `NV21` / `UYVY` / `NV12` start/stop cycles | 30/30, no sequence gaps |
| Legacy Core started after an `NV21` session | logged-in web page H.264 1080p 60 FPS, picture correct |

## Source

`interdrv/vi/chip/cv181x/vi_v4l2.c` in `osdrv-sg200x`; `vi.c` provides the
exclusion against the character device (`vi_v4l2_acquire` /
`vi_v4l2_release`) and the CIF call. The node is built when the kernel has
`VIDEOBUF2_V4L2`.
