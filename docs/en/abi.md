# OneKVM backend ABI

English | [简体中文](../zh/abi.md)

`soph-mmf.so` exports exactly two symbols. See `src/onekvm_backend.map`:

| Symbol | Header | Role |
|--------|--------|------|
| `onekvm_video_backend_query` | `include/onekvm/video_backend_v1.h` | Capture, encode, EDID, snapshots |
| `onekvm_crypto_backend_query` | `include/onekvm/crypto_backend_v1.h` | Optional SRTP TX AES-GCM |

This is a C ABI. Structures are append-only; callers and the backend both
read `struct_size` first. Vendor MMF symbols stay hidden and never enter
Core's dynamic link set.

`abi_version` is `1` for both tables. An incompatible request fails at
query time so Core can reject the backend during load.

## Video

`driver_id` is `soph-mmf`. Codec mask: `auto`, H.264, H.265, MJPEG.

### Feature bits

| Bit | Meaning |
|-----|---------|
| `SIGNAL_PRESENT` | `source_signal_present` |
| `KEYFRAME` | `encoder_request_keyframe` |
| `BORROWED_PACKET` | `encoder_release_packet` |
| `PREPARE_ENCODE` | `encoder_prepare` (async hand-off on the manual path) |
| `BOUND_ENCODER` | `encoder_bind_source` / `encoder_read_packet` / `encoder_unbind_source` |
| `LATENCY` | `source_latency` / `encoder_latency`, cached samples only |
| `EDID` | `edid_capabilities` / `edid_get` / `edid_set`; no source handle |
| `ENCODER_ALLOCATION` | `encoder_resources` / `encoder_allocate` / `encoder_allocation` |
| `SOURCE_SNAPSHOT` | `source_snapshot` |

`INTERRUPT_READ` is not implemented (`source_interrupt` is `nullptr`).
Status/SSE paths must not `GetFrame`, `GetStream`, or dump procfs.

### Advertised formats

The ABI table lists common output profiles. It is not an HDMI input
whitelist:

```text
2880x1620@30  2560x1440@30  1920x1080@60  1280x720@60  640x480@60
```

Pixel format is NV21. Input acceptance and the full selectable list are in
[video-pipeline.md](video-pipeline.md) and
`include/input_resolution_tracker.hpp`.

### Bound vs manual

The KVM live path is **bound**: hardware connects VPSS to VENC, and Core
only calls `encoder_read_packet`. Do not call `source_read` or
`encoder_encode` on that path.

The manual path (`encoder_encode` / `source_read`) is an internal fallback,
for example when another feature also needs a raw frame and may copy one
NV21 buffer. It is not a second public API. Do not `SendFrame` on a live
bound encoder.

### Encoder leases

`encoder_resources` policy is currently one H.26x plus one JPEG.
`hardware_channel_capacity` is the vendor channel count. Callers pick a
policy and input shape, never a hardware channel number.

- `REALTIME`: the KVM live path.
- `BACKGROUND`: does not preempt the realtime encoder. An idle KVM session
  does not mean the encoder is destroyed. Background compositing needs an
  explicit Core ownership hand-off.

`encoder_create` still exists for older callers. New managed paths use
`encoder_allocate`.

### Snapshots

`source_snapshot` reuses the existing VPSS, allocates a JPEG encoder, and
writes the result into the caller's buffer.

- `timeout_ms` max is 1000; 0 means the default 250 ms.
- Explicit width/height must match the current output, or both must be 0.
- Confirmed no-signal / `out_of_range` returns `UNSUPPORTED` and does not
  JPEG-encode the placeholder artwork.
- Callers must not open a second MMF source just to take a snapshot.

### EDID

Library-level; no source handle. After a successful `edid_set`,
`apply_required` tells the host whether to hotplug or reboot. The library
does not `reboot`.

- PCIe: writable + hotplug (`apply_policy=HOTPLUG`).
- Cube / Lite: writable + persistent, `apply_policy=REBOOT`.

## Crypto

`backend_name` is `nanokvm CryptoDMA AES-GCM`. Device node
`/dev/onekvm-crypto-offload`.

Advertised: `AES_GCM_TX`, `AES_GCM_TX_BATCH`.
Do **not** advertise `CONCURRENT_H265_VIDEO`: concurrent H.265 VENC and
CryptoDMA hard-locks the SG2002, so Core uses software AES-GCM for H.265
sessions.

Completion is a poll of `CRYPTODMA_WR_INT`. Do not `wait_event` on PLIC 59
from the live DTB; that interrupt never fires. The first ioctl `ETIMEDOUT`
disables offload for the rest of the process so the video thread does not
stall at 1 FPS.
