# SG2002 V4L2 scaler and format converter

[简体中文](../zh/v4l2-scaler.md)

`soph_vpss` registers the memory-to-memory node `soph-vpss`. It crops, scales,
flips and converts frames on the VPSS hardware. Sessions create their VPSS group
through the in-kernel VPSS layer, without cvi_mpi.

`soph-vi` can output NV21/NV12 itself (see
[v4l2-capture.md](v4l2-capture.md#420-output)), so same-size encoding does not
need this node. Scaling, cropping, flipping and other formats go through it.

Status: experimental. Validated on device 107 (6.18.57-onekvm-preempt) for:

- formats, sizes, crop and flips;
- chaining with the capture and encoder nodes;
- running alongside the legacy Core.

Core does not use it yet.

## Node

| Node name | Type | Formats (same for OUTPUT and CAPTURE) | Size |
| --- | --- | --- | --- |
| `soph-vpss` | `VIDEO_M2M_MPLANE`, `STREAMING`, single-plane buffers | `NV12` `NV21` `NV16` `NV61` `YU12` `UYVY` `YUYV` `VYUY` `YVYU` `GREY` `RGB3` `BGR3` | 32×32 to 2880×4096 |

The OUTPUT default is UYVY 1920×1080 and the CAPTURE default is NV12 1920×1080.
`videoN` numbers are dynamic. Find the node by name, or by the `card`
(`SG200x VPSS scaler`) from `VIDIOC_QUERYCAP`.

- Multi-plane formats store their planes one after another in one buffer, so
  `num_planes` is 1.
- Width and height must be even for formats with subsampled chroma. `GREY` and
  RGB have no such limit.
- The downscale ratio must stay above 1/32:
  - `S_SELECTION` adjusts the rectangle to fit.
  - A ratio of 1/32 or below set through `S_FMT` makes `STREAMON` return `EINVAL`.

  Upscaling has no such limit.
- A CAPTURE wider than 1920 uses the `sc_v1` channel; otherwise `sc_d` is used.
  Both run on VPSS `img_v` and share the hardware with the legacy Core's groups.

## Line stride and buffers

| Queue | `bytesperline` |
| --- | --- |
| OUTPUT, packed YUV (`UYVY` etc.) | at least `ALIGN(width × 2, 16)`, as `soph-vi` writes it; a wider value is accepted (up to 2×, 16-byte aligned) |
| OUTPUT, other formats | at least the minimum stride, 64-byte aligned; a wider value is accepted (up to 2×) |
| CAPTURE | chosen by the node, 64-byte aligned (e.g. 1280 for NV12 at 1280 wide, 3840 for RGB3 at 1280 wide) |

Buffers can be `MMAP` or `DMABUF`. When queuing `DMABUF`:

- set `planes[0].length` to the buffer size;
- set `bytesused` to at least `sizeimage`;
- `data_offset` must be a multiple of 64.

`MMAP` with `V4L2_MEMORY_FLAG_NON_COHERENT` gives a cached mapping.

## Session

1. `S_FMT` OUTPUT first, then CAPTURE. `S_FMT` returns `EBUSY` while buffers are
   allocated, and resets that queue's crop or compose rectangle to the full frame.
2. Optional settings:
   - `S_SELECTION`: `V4L2_SEL_TGT_CROP` on OUTPUT selects the source area.
     `V4L2_SEL_TGT_COMPOSE` on CAPTURE places the picture in the output frame and
     fills the rest with black.
   - `V4L2_CID_HFLIP` / `V4L2_CID_VFLIP` flip the picture.

   All of these can also change while streaming and apply from the next frame.
3. The node creates its VPSS group once both queues stream. `STREAMOFF` on either
   queue destroys the group.
4. Each OUTPUT buffer produces one CAPTURE buffer:
   - The timestamp, `field` and timecode are copied from OUTPUT to CAPTURE, so
     the `soph-vi` frame-end time reaches the encoder output.
   - A frame that fails returns with `V4L2_BUF_FLAG_ERROR`.
   - A frame that does not finish within 1 s is dropped and the group is
     recreated. Later frames continue.

## Colour

- YUV input defaults to BT.709 limited range; RGB defaults to sRGB full range.
  Colour fields set with `S_FMT` are kept.
- YUV↔RGB conversion always uses BT.601 coefficients and keeps the value range:
  - limited-range YUV gives RGB in 16–235;
  - full-range RGB gives YUV in 0–255.
- CAPTURE reports the OUTPUT `quantization` and a `ycbcr_enc` to match.

## Running alongside cvi_mpi

- The node takes VPSS groups from the top (15) down, leaving the low groups the
  legacy Core uses.
- Character device (`/dev/cvi-vpss`) ioctls on these groups return `EPERM`, so
  the legacy Core's stale-group reclaim skips them.
- Both can run at once; the VPSS hardware alternates between them frame by frame.

## Statistics

`/sys/class/video4linux/videoN/stats`:

| Field | Meaning | Scope |
| --- | --- | --- |
| `frames` | completed frames | cumulative |
| `errors` | failed frames | cumulative |
| `timeouts` | timed-out frames | cumulative |
| `process_us_*` | hardware processing time: average and sum | cumulative |
| `process_us_max` | hardware processing time, maximum | current stream |
| `latency_us_*` | OUTPUT `QBUF` to CAPTURE completion: average and sum | cumulative |
| `latency_us_max` | OUTPUT `QBUF` to CAPTURE completion, maximum | current stream |

## Device 107 measurements (2026-11-11)

All measurements use `v4l2-scale-smoke`, installed with the module package in
`/usr/libexec/onekvm/`.

### Synthetic colour bars

`-t`: Core stopped, MMAP input of 1920×1080 colour bars, 60 frames.

| Case | Result |
| --- | --- |
| UYVY → all 12 output formats (720p) | all pass; deviation 0 for YUV outputs, ≤ 1 for RGB outputs (expected values follow the colour rules above) |
| NV12, RGB3 and YUYV input | colour bar deviation 0 |
| Hardware time per frame, 1080p → 1080p/720p NV12 | about 6.2 ms, about 150 fps |
| 1080p → 2560×1440 / 2880×1620 | 93 / 75 fps |
| 1366×768 → 720p | 270 fps |
| 32×32 → 1024×1024; 1080p → 62×36 | correct |
| 1080p → 60×34 (1/32) | no output; each frame timed out after 1 s and the group was recreated, later frames correct; the node now rejects this ratio |
| Horizontal flip, crop, compose | correct |

### Chained with capture and encoders

Host 99 outputs 1920×1080@60, Core stopped. Latency is measured from VI frame
end.

| Case | Result |
| --- | --- |
| `soph-vi` → `soph-vpss` NV12 1080p, 600 frames | 60.00 fps, no VI sequence gaps; to scaled `DQBUF` avg 6.66 ms, P95 6.79, max 6.86 |
| → NV12 720p | 60 fps; avg 6.65 ms, P95 6.79, max 6.83 |
| → NV12 1080p → `soph-venc` H.264, 4 runs of 600 frames | 60 fps (59.4 on the first run); to bitstream `DQBUF` avg 16.4–17.4 ms, P50 16.4, P95 16.5–22.8, max 23–157 |
| → NV12 720p → H.264 | 60 fps; avg 11.62 ms, P95 11.63, max 31.23 |
| → NV12 1080p → H.265 | 60 fps; avg 17.42 ms, P95 17.51, max 27.28 |
| Reference: UYVY straight to H.265 (no VPSS, same session) | 60 fps; avg 10.28 ms, P95 10.30, max 19.71 |
| → NV12 720p → JPEG, 120 frames | 60 fps; avg 9.87 ms, P95 10.03 |
| Centre 960×540 crop → 1080p H.264 | 60 fps; avg 19.06 ms, P95 19.11 |
| Bitstream decode | FFmpeg decodes every frame without errors; scaled and cropped pictures correct |
| 10 start/stop cycles | 10/10 |
| Legacy Core started during a 40 s synthetic stream | synthetic stream 0 errors, 0 timeouts, average latency 24.5 → 26.8 ms; Core starts normally |
| Legacy Core on these modules | logged-in web page H.264 1080p 60 FPS |

- Going through VPSS adds about 7 ms over UYVY straight to H.265: the time VPSS
  takes for a whole frame. It depends only on the pixel count: with `soph-vi`
  writing NV21 instead, the scaled 1080p / 720p frames still average 6.68 /
  6.67 ms, and 720p H.264 P50 stays at 11.53 ms.
- The legacy path records a VPSS `CostTime` of about 6.1 ms (see
  the Chinese [video-pipeline.md](../zh/video-pipeline.md)), the same order. That figure comes from
  the older document, not from this session.
- The synthetic test queues 4 frames at once, so its `QBUF` → `DQBUF` time
  includes queueing. Use `process_us` for processing time.

## Source

`interdrv/vpss/common/vpss_v4l2.c` in `osdrv-sg200x`:

- `vpss.c` provides the kernel groups (`vpss_create_kernel_grp`) and the
  completion callback.
- `vpss_ioctl.c` refuses character device access to those groups.

The node is built when the kernel has `V4L2_MEM2MEM_DEV`.
