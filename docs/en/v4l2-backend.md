# V4L2 video backend (soph-v4l2.so)

[简体中文](../zh/v4l2-backend.md)

`soph-v4l2.so` implements the `onekvm::mmf` interface in `src/mmf.hpp` on the
kernel V4L2 nodes. The upper layers (ABI, encoders, snapshots, LT6911, EDID,
no-signal frames) are the same sources as in `soph-mmf.so`. It links only
libc, libstdc++ and libgcc, does not load cvi_mpi, and does not need
`onekvm-video-soph-mmf-runtime`.

Status: experimental. Validated on device 107 (6.18.57-onekvm-preempt) for
function, latency, resources and memory. The device descriptor still loads
`soph-mmf.so` by default.

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

ION on device 107 (lazy-cma, 1080p60 H.264):

| State | soph-mmf.so | soph-v4l2.so |
| --- | --- | --- |
| Idle | 0.3 MB / 1 buffer | 0 |
| Streaming 1080p60 | 39.2 MB | 32.5 MB / 12 buffers |
| Idle after streaming | 8.3 MB (VENC buffers) | 0 (3 rounds in a row) |

With MJPEG as the primary codec, the upper layers unbind the primary JPEG
encoder's source when the last MJPEG client disconnects, and the backend
then pauses VI and the scaling channel. That call used to do nothing: both
backends kept their capture buffers until H.26x was used again or the
service restarted. Unbinding a JPEG encoder that VNC bound logically leaves
capture running (no VNC on device 107; not measured).

Device 107 (1080p60, one HTTP MJPEG client for 8 s, lazy-cma ION):

| Backend | Streaming | 15 s / 30 s after disconnect (before) | 15 s / 30 s after disconnect (after) |
| --- | --- | --- | --- |
| soph-v4l2.so | 25.5 MB / 8 buffers | 25.5 / 25.5 MB | 0 / 0 |
| soph-mmf.so | 32.2 MB / 5 buffers | 32.2 / 32.2 MB | 1.3 / 1.3 MB |

## Device 107 measurements

Conditions: device 107 (SG2002, 6.18.57-onekvm-preempt); host 99 outputs
1920×1080@60; one `onekvm.service` client; CPU measured without a browser.
Both backends come from the same recipe build. Runs alternate between them by
changing the descriptor `library` and restarting the service.

### End-to-end latency

Boundary: an HTTP trigger flips a full-screen page on the host between black
and white → host 99 display → HDMI → 107 → WebRTC → headless Chromium polls the
decoded picture (2 ms) and detects the flip. Timed on one Chromium monotonic
clock; no cross-machine clocks are involved. 40 valid samples per run; values
in ms.

| Output | Backend | Runs×samples | Video P50 | P95 | Mean | Keyboard closed loop P50 / P95 |
| --- | --- | --- | --- | --- | --- | --- |
| 1920×1080 (direct) | soph-mmf.so | 3×40 | 58.8 | 62.3 | 58.6 | 59.2 / 72.7 |
| 1920×1080 (direct) | soph-v4l2.so | 3×40 | 58.7 | 62.3 | 58.6 | 58.3 / 62.2 |
| 1280×720 (scaled) | soph-mmf.so | 5×40 | 48.9 | 61.5 | 52.6 | 59.3 / 62.6 |
| 1280×720 (scaled) | soph-v4l2.so | 3×40 | 59.3 | 62.3 | 55.5 | 60.2 / 63.6 |

- Samples fall into two modes, about 47 ms and about 60 ms, depending on the
  phase between frame completion and the display refresh. Which mode holds the
  P50 depends on the share of low-mode samples, so compare the means and the
  shares. At 720p the low-mode share is 52% for soph-mmf.so and 32% for
  soph-v4l2.so, and the mean is 2.9 ms higher. At 1080p both are the same
  (0% vs 4%).
- The soph-mmf.so 1080p keyboard closed-loop P95 includes a stall in one run
  (max 351 ms); the video P95 is unaffected.
- The extra time on the scaled path comes from two hand-offs through the
  pipeline thread between VI → `soph-vpss` → `soph-venc`. VPSS itself is not
  slower: queue to completion is 6.44 ms, of which the hardware takes 6.19 ms.
  Raising the pipeline thread to nice -20 gave a 3×40 mean of 57.2 ms, no
  improvement, so it was not kept.

The device-side items of the Web UI performance panel are measured differently
by the two backends and are for reference only (soph-v4l2.so vs soph-mmf.so):
1080p capture 17 vs 23 ms, encode 9.6 vs 9.1 ms; 720p capture 24 vs 23 ms,
encode 4.8 vs 4.2 ms.

### Resources

Moving content (host page `?motion=1`), 1080p60 H.264 over WebSocket, no
browser. SG2002 has one core; process CPU counts one core as 100%. Each run
samples 15 s; three alternating runs per backend:

| Metric | soph-mmf.so | soph-v4l2.so | Change |
| --- | --- | --- | --- |
| Process CPU | 13.8 / 13.9 / 13.8% | 12.0 / 11.7 / 12.3% | −1.8 percentage points (−13%) |
| Received frame rate | 60.0 FPS, no gaps | 60.0 FPS, no gaps | none |
| Bitrate | 4.09 / 3.60 / 4.06 Mbps | 1.28 / 1.10 / 1.14 Mbps | picture quality on moving content not compared frame by frame |
| RSS | about 33 MB | about 35–36 MB | +2–3 MB |

The bitrates differ, so the CPU difference includes the difference in what
Core sends.

### Startup and snapshots

H.264 WebSocket startup to the first key frame, and a single JPEG snapshot
taken 12 s after streaming stops; three rounds each at 1080p and 720p:

| Item | soph-mmf.so median (range) | soph-v4l2.so median (range) |
| --- | --- | --- |
| Startup | 0.225 s (0.21–0.25) | 0.215 s (0.20–0.25) |
| Idle snapshot | 0.331 s (0.305–0.353) | 0.308 s (0.288–0.481) |

### Bitrate cap

1080p60 H.264 over WebSocket. The cap is changed through the config API
(`video.h26x.bitrate_kbps`; explicit QPs must be cleared to 0 first), 10 ↔ 1
Mbps six times each way at random GOP phases. Delay = from the API reply to the
first P frame whose QP moves 3 or more toward the new target:

| Backend | Direction | Before osdrv `495d985`, median (range) | After, median (range) |
| --- | --- | --- | --- |
| soph-v4l2.so | 10 → 1 Mbps | 3.40 s (0.84–4.50) | 0.16 s (0.02–0.33) |
| soph-v4l2.so | 1 → 10 Mbps | 1.93 s (0.17–4.43) | 0.26 s (0.24–1.16) |
| soph-mmf.so | 10 → 1 Mbps | 2.95 s (1.12–4.99) | 0.07 s (0.04–0.32) |
| soph-mmf.so | 1 → 10 Mbps | 3.53 s (0.73–4.70) | 0.05 s (0.00–0.11) |

Before, H.264 took a new bitrate only at the next natural GOP start (up to 5 s
with a 300-frame GOP). After the change, soph-v4l2.so 1080p end-to-end latency
over 2×40 samples is P50 58.6, P95 62.5, mean 58.9 ms (before, 3×40: 58.7 /
62.3 / 58.6 ms).

Steady-state bitrate with each cap held for 10 s or more (old module; the
change does not affect the steady state):

| Content | Cap | soph-mmf.so bitrate (P-frame QP) | soph-v4l2.so bitrate (P-frame QP) |
| --- | --- | --- | --- |
| Desktop | 10 / 3 / 1 Mbps | not measured | 2.5 / 0.79 / 0.17 Mbps (13 / 26 / 48) |
| Moving page | 10 / 5 / 1 Mbps | 3.6 / 3.4 / 2.5 Mbps (18 / 20 / 48) | 1.1 / 1.1 / 1.9 Mbps (14 / 19 / 48) |

At 1 Mbps the moving page already has QP at its 48 limit, and neither backend
gets down to the cap. soph-v4l2.so then runs higher than with no effective cap:
at QP 48 about 1780 of 8160 macroblocks per P frame are intra coded, against
about 900 at QP 14.

Both backends now turn on frame skip whenever a cap is set: once the bitrate
buffer exceeds about 1.1 s (H.264) / 1.2 s (H.265) of the cap, the next frame
is coded as a skipped picture. The vendor rate control used to count skipped
pictures as coded ones, so QP dropped to 18–20 instead, at about 1 frame/s
with runs of 100–210 skipped pictures. Since osdrv `ed61172` skipped pictures
count only toward the bitrate buffer.

Quality test page (text and shapes moving every frame), 1080p60 H.264 over
WebSocket, each cap held 15 s with the last 10 s used, 2 rounds each; before =
a backend without frame skip:

| Backend | Cap | Bitrate before | Bitrate after | Coded frame rate after | P-frame QP after | Longest skip run after |
| --- | --- | --- | --- | --- | --- | --- |
| soph-v4l2.so | 10 Mbps | 7.70–8.02 Mbps | 7.98–8.27 Mbps | 60 | 18–23 | 0 |
| soph-v4l2.so | 3 Mbps | 2.32–2.45 Mbps | 2.30–2.48 Mbps | 60 | 40 | 0–1 |
| soph-v4l2.so | 1 Mbps | 1.29–1.48 Mbps | 0.90–0.91 Mbps | 6.1–6.2 | 48 | 20 |
| soph-v4l2.so | 500 kbps | 1.33–1.67 Mbps | 0.46–0.47 Mbps | 3.0 | 48 | 33–43 |
| soph-mmf.so | 10 Mbps | 7.70–8.05 Mbps | 7.94–8.47 Mbps | 60 | 20–24 | 0 |
| soph-mmf.so | 3 Mbps | 2.26–2.42 Mbps | 2.39–2.53 Mbps | 60 | 45–46 | 0 |
| soph-mmf.so | 1 Mbps | 1.66–1.72 Mbps | 0.91 Mbps | 6.0–6.2 | 48 | 17–19 |
| soph-mmf.so | 500 kbps | 1.70–1.97 Mbps | 0.46 Mbps | 2.9 | 48 | 34–38 |

The cost: with heavy motion and a low cap the coded frame rate falls to a few
frames per second, and the frame after a skip carries more accumulated change,
so it is larger.

Automatic bitrate over a throttled link: the WebSocket client link is shaped to
10 s unlimited, 30 s at 1.5 Mbps, 20 s unlimited, on the quality test page;
soph-v4l2.so before and after, 2 alternating rounds each. The throttled figures
use its last 20 s:

| Item | Before (no frame skip) | After |
| --- | --- | --- |
| Bitrate | 1.47 Mbps (fills the link) | 0.96–1.01 Mbps |
| Coded frame rate | 55–63 FPS | 7.8–8.2 FPS |
| Queueing delay P50 | 2.6 / 4.9 s | 0–1 ms |
| Queueing delay P95 (max) | 2.8 / 5.5 s (2.9 / 6.0 s) | 0.16 / 3.3 s (1.0 / 4.0 s) |
| Backlog | never clears while throttled | clears 10–12 s after throttling starts |

The first 10 s of throttling look alike: the backlog grows to 3–6 s before
automatic bitrate lowers the cap.

### Function

| Item | Result |
| --- | --- |
| H.264 1080p60 WebRTC (Web UI) | 60 FPS |
| H.265 WebSocket | 60 FPS, bitstream decodes without errors |
| MJPEG | 464 frames in 8 s (soph-mmf.so 479) |
| 720p scaled output | 60 FPS, 1280×720 |
| HDMI 1080p → 480p → no signal → 1080p while streaming | No gaps, correct size per segment, placeholder shown |
| 10 start/stop cycles (1080p/720p alternating) | 60 FPS each time; fd and ION counts do not grow |
