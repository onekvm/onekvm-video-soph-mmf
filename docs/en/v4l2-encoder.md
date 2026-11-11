# SG2002 V4L2 encoder

[简体中文](../zh/v4l2-encoder.md)

`soph_vc_driver` registers two V4L2 memory-to-memory encoder nodes that call
the in-kernel VENC channel layer directly, without cvi_mpi. They are the
encoding part of moving the backend off cvi_mpi; capture and scaling nodes are
separate work. The existing MMF pipeline (character devices plus cvi_mpi)
remains available, and both can run at the same time.

Status: experimental, validated on device 107 (6.18.57-onekvm-preempt) for
encoding, concurrency and bitstream decoding. Core does not use it yet.

## Nodes

| Node name (`/sys/class/video4linux/videoN/name`) | CAPTURE bitstream | OUTPUT raw formats | Size |
| --- | --- | --- | --- |
| `soph-venc` | `H264`, `HEVC` | H.264: `NV12`, `NV21`; H.265 also `UYVY`, `YUYV` | 64×64 to 2880×1620, even |
| `soph-jpegenc` | `JPEG` | `NV12`, `NV21`, `UYVY`, `YUYV` | 64×64 to 4096×4096, even |

`videoN` numbers are dynamic; find the node by name or by the `card` from
`VIDIOC_QUERYCAP`. Both nodes are single-plane MPLANE (`num_planes = 1`);
the NV12/NV21 chroma plane follows the luma plane with the same stride.

## Session

1. `S_FMT` CAPTURE first to choose the bitstream format (the OUTPUT formats on
   offer follow it), then `S_FMT` OUTPUT for size and raw format. The OUTPUT
   stride is at least `ALIGN(width × bytes per pixel, 32)`; a larger 16-byte
   aligned stride is accepted (for example VI's `ALIGN(width × 2, 16)`).
2. `S_PARM` (OUTPUT) sets the input frame rate; rate control follows it.
3. A VENC channel is taken and encoding starts once both queues are streaming;
   `STREAMOFF` on either queue or closing the file releases it. A new session
   starts with an IDR.
4. Each OUTPUT buffer produces one CAPTURE access unit (complete Annex B for
   H.26x with parameter sets before IDRs; a complete image for JPEG). CAPTURE
   buffers carry `KEYFRAME` / `PFRAME` and the OUTPUT timestamp.
5. `V4L2_ENC_CMD_STOP` drains queued input; the last CAPTURE carries `LAST` and
   `V4L2_EVENT_EOS` is raised. `V4L2_ENC_CMD_START` resumes with an IDR.

When a frame fails, its OUTPUT buffer returns with `ERROR` and no CAPTURE
buffer is dequeued; the driver resets the channel and forces an IDR next. If
the reset fails the session stops encoding and must be reopened.

Only one H.26x V4L2 session runs at a time (a second `STREAMON` returns
`EBUSY`). V4L2 takes VENC channels from the highest number down; channels that
cvi_mpi has open are skipped, and opening the character device of a channel
V4L2 holds returns `EBUSY`.

## Buffers

| Queue | Memory | Modes |
| --- | --- | --- |
| OUTPUT | Physically contiguous ION (`onekvm_lazy_cma`, carveout fallback), write-combined CPU mapping | `MMAP`, `DMABUF` import/export |
| CAPTURE | vmalloc; the driver copies the VENC bitstream in | `MMAP` |

The hardware reads OUTPUT in place. Imported DMA-BUFs must be physically
contiguous and below 4 GiB. CPU pixel writes to OUTPUT are slow (write-combined,
uncached); queue buffers exported by the capture or scaling nodes as DMA-BUFs
instead. CAPTURE defaults to `width × height / 2` (1–4 MiB for H.26x,
512 KiB–4 MiB for JPEG); `sizeimage` on `S_FMT` CAPTURE selects 64 KiB–16 MiB.
An access unit that does not fit counts as a failed frame.

## Controls

| Control | Range / default | Applies |
| --- | --- | --- |
| `MPEG_VIDEO_BITRATE` | 16 kbit/s–409.6 Mbit/s, default 4 Mbit/s | next frame |
| `MPEG_VIDEO_BITRATE_MODE` | CBR / VBR (default); H.265 VBR is AVBR | session start |
| `MPEG_VIDEO_FRAME_RC_ENABLE` | off selects fixed QP (I/P QP controls) | session start |
| `MPEG_VIDEO_GOP_SIZE` | 1–65535, default 150 | next frame |
| `MPEG_VIDEO_FORCE_KEY_FRAME` | — | next frame |
| `MPEG_VIDEO_H264_PROFILE` | Baseline (CAVLC, default) / Main / High (CABAC) | session start |
| `MPEG_VIDEO_{H264,HEVC}_{MIN,MAX,I_FRAME,P_FRAME}_QP` | 0–51, default 12 / 48 / 22 / 24 | next frame |
| `JPEG_COMPRESSION_QUALITY` (JPEG node) | 1–100, default 85; 100 maps to 99, 50 to 51 | next frame |

B frames are fixed at 0; parameter sets always accompany IDRs.

## Statistics

`/sys/class/video4linux/videoN/stats` reports cumulative frames, bytes,
errors, timeouts, skipped frames and resets, plus encode time (SendFrame until
the bitstream is collected) and latency (OUTPUT `QBUF` until done) as average,
sum, this stream's maximum and a 0.5 ms histogram. Compute a test window from
before/after deltas.

## Device 107 measurements (2026-11-11)

`v4l2-encode-smoke` (installed with the module package in
`/usr/libexec/onekvm/`) with a synthetic pattern at 1920×1080, 3 OUTPUT and
4 CAPTURE buffers, Core stopped:

| Case | Throughput | Latency (QBUF→DQBUF, ms) |
| --- | --- | --- |
| H.264 NV12, unthrottled | 103.7 fps | — |
| H.265 NV12 / UYVY, unthrottled | 98.3 / 99.2 fps | — |
| JPEG NV12 / UYVY, unthrottled | 224.6 / 171.5 fps | — |
| H.264 NV12, 60 fps real time | 60.0 fps | avg 9.56, P95 9.65, max 13.80 |
| H.265 UYVY, 60 fps real time | 60.0 fps | avg 10.10, P95 10.13, max 14.98 |

Draining with `ENC_CMD_STOP` returned `LAST` and `EOS` for H.264, H.265 and JPEG.

Every output decoded completely with host FFmpeg without errors and matched the
input pattern. With Core streaming 1080p60 through cvi_mpi, concurrent V4L2
JPEG 1080p30 and H.265 720p30 ran with zero errors and the browser stayed at
about 60 fps.

## Source

`interdrv/cvi_vc_drv/cvi_venc_v4l2.c` in `osdrv-sg200x`. The shared ION vb2
memory layer is `interdrv/include/common/kapi/onekvm_vb2_ion.h`, for reuse by
the capture and scaling nodes. The kernel needs `V4L2_MEM2MEM_DEV` and
`VIDEOBUF2_VMALLOC` (as for the decoder).
