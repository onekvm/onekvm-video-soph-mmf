# SG2002 V4L2 capture

[简体中文](../zh/v4l2-capture.md)

`soph_vi` registers the V4L2 capture node `soph-vi`: MIPI-rx (CIF) through the
VI YUV bypass DMA, writing UYVY, or NV21/NV12 converted by the CSI bridge while
writing. This is the path the LT6911 HDMI bridge feeds.
`STREAMON` programs CIF and the VI device, pipe and channel through the
in-kernel SDK layer, without cvi_mpi. The bridge (timing, EDID, I2C) stays in
user space, which sets the current active size with `S_FMT` before `STREAMON`.

Status: experimental. The `soph-v4l2.so` backend uses this interface.
See [V4L2 video backend](v4l2-backend.md).

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

## Source

`interdrv/vi/chip/cv181x/vi_v4l2.c` in `osdrv-sg200x`; `vi.c` provides the
exclusion against the character device (`vi_v4l2_acquire` /
`vi_v4l2_release`) and the CIF call. The node is built when the kernel has
`VIDEOBUF2_V4L2`.
