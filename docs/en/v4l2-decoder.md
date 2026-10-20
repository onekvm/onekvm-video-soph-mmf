# SG2002 V4L2 decoder

[简体中文](../zh/v4l2-decoder.md)

This interface accepts compressed access units and outputs raw frames. HDMI
capture, VPSS scaling, hardware encoding and Core's encoding backend retain
their MMF pipeline. It does not add a V4L2 encoder node.

Status: experimental implementation, with basic validation completed on device
107 running 6.18.45-onekvm-preempt. It is not a replacement for the encoding
path; a successful build or registered node does not establish all stream
formats or resource cleanup.

## Interface

Use the stable public alias `/dev/onekvm-vdec`; it hides the vendor driver name
and the dynamically allocated `videoN` number. The module loader and udev rule
maintain this alias. If it is unavailable, enumerate `/dev/video*` at startup
and use `VIDIOC_QUERYCAP` to select a node whose driver name ends in `-vdec`
and whose capabilities include `V4L2_CAP_VIDEO_M2M_MPLANE | V4L2_CAP_STREAMING`.

| Compressed OUTPUT | Raw CAPTURE |
| --- | --- |
| H.264 Annex-B, complete access units, I/P pictures | `V4L2_PIX_FMT_YUV420M` |
| 8-bit baseline JPEG/MJPEG | `YUV420M`, `YUV422M`, `YUV444M`, or `GREY`, following the stream |

HEVC decoding is not exposed. Access units are limited to 2 MiB. Visible
dimensions range from 32×32 to 2880×1620. Supported performance and dimensions
need separate hardware validation; format enumeration is not a throughput
guarantee.

CAPTURE dimensions are aligned to 16 pixels. `VIDIOC_G_SELECTION(COMPOSE)`
reports the visible image. JPEG uses full-range JPEG/BT.601 metadata. H.264
currently defaults to BT.709; stream VUI color metadata is not propagated.

Queues support MMAP and the DMA-BUF import/export provided by vb2-vmalloc.
Hardware reference pictures use decoder-owned ION memory. Visible rows are
copied into vb2 buffers, so decoding is not zero-copy.

## Streaming

1. Configure, allocate, queue and start OUTPUT with complete access units.
2. Subscribe to `V4L2_EVENT_SOURCE_CHANGE`, then query CAPTURE and COMPOSE.
3. Allocate and queue the CAPTURE planes and start CAPTURE to receive frames.
4. On resolution or JPEG sampling changes, drain old CAPTURE buffers through
   `LAST`, stop/free CAPTURE, query the new format, and recreate CAPTURE while
   retaining the OUTPUT session.
5. `V4L2_DEC_CMD_STOP` drains queued input and returns an empty CAPTURE `LAST`
   buffer plus `V4L2_EVENT_EOS`. New OUTPUT `QBUF` requests are rejected while
   draining.
6. `V4L2_DEC_CMD_START` resets decoder state after draining. Subsequent input
   must contain its required parameter sets. Closing or stopping OUTPUT
   releases the decoder, reference pictures and session reservation.

Only one active decoder session is allowed. The SG2002 H.264/H.265 cores share
SRAM, so H.264 decoding and H.265 encoding exclude each other. A shared session
creation gate rejects the conflict before initializing hardware. Encoding
parameters, packet handling and MMF capture are unchanged by this gate.

JPEG headers, precision, dimensions and SOI/EOI are checked before allocating
separate uncached ION output. `GetFrameData` releases the JPU transaction lock;
output memory survives until the CAPTURE copy. Waiting for userspace to
recreate CAPTURE does not retain the shared JPU lock.

## Implementation and validation

Linux `drivers/media/platform/sophgo/sophgo-vdec.c` owns V4L2, vb2, events and
queues. `include/media/sophgo-vdec.h` defines the kernel backend operations;
`osdrv-sg200x/interdrv/cvi_vc_drv/cvi_vdec_v4l2.c` supplies H.264/JPEG hardware
operations.

`CONFIG_VIDEO_SOPHGO_VDEC=m` selects the media modules packaged as
`onekvm-device-nanokvm-kernel-module-video-decoder`. The vendor module package
depends on it. Build target code through `onekvm-distro` OE/kas debug worktree
entry points, not a standalone cross compiler.

The module package includes `/usr/libexec/onekvm/v4l2-decode-smoke`. It uses
public V4L2 ioctls, prefers `/dev/onekvm-vdec` and falls back to a `*-vdec`
node, reads repeated
little-endian `uint32_t` packet lengths plus compressed access units, and writes
visible raw planes without coded padding. Its command format is
`v4l2-decode-smoke h264|mjpeg|jpeg packets.bin raw.yuv`, with optional hold and
restart counts. Compare its output against software decoding of the identical
bitstream.
Validation also needs EOS, close/reopen, resolution changes, invalid input,
contention and regression checks of the existing WebRTC picture.
On 107, H.264 640×480 → 640×360 → 1920×1080 (30 frames total), JPEG 4:2:0,
standard 4:2:2, standard 4:4:4, grayscale, sampling changes, and truncated,
invalid and oversized inputs were exercised. After restoring Core, the
existing WebRTC stream remained 1920×1080 H.264 at 60 FPS. A grayscale/color
CAPTURE plane-count change requires stopping and reallocating the queue as soon
as the event arrives; old plane-count buffers must not be QBUF'd afterward.
