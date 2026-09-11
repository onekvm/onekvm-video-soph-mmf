# onekvm-nanokvm-mmf

English | [简体中文](README.zh-CN.md)

NanoKVM video and SRTP crypto backend for OneKVM. It connects the SG2002
multimedia pipeline to the OneKVM backend ABI and keeps vendor MMF code out of
`onekvm-server`.

Device builds go through `onekvm-distro` OpenEmbedded/kas. Do not cross-compile
this tree outside that distro.

## Features

- Captures HDMI from the LT6911.
- Hardware-encodes H.264, H.265, and MJPEG.
- Accepts even HDMI timings from 320×200 through 2880×1620, up to 5 MP, within
  a 5 MP at 30 FPS (150M pixel/s) budget. 4K input is unsupported.
- Advertises output profiles through 2880×1620@30. 1080p is up to 60 FPS; 720p
  can follow a 120 Hz HDMI source.
- Bound encoder path: VPSS feeds VENC directly. Core reads compressed access
  units, not raw 1080p frames.
- Reports HDMI signal state, sink EDID, capture/encode latency, and a built-in
  no-signal frame for the raw-frame fallback.
- Optional AES-GCM offload through the CVITEK SPACC character device.

The Cube capture path, CSI bridge matching, HDMI watcher, VENC reader, idle
teardown, and snapshot rules are in
[docs/video-pipeline.md](docs/video-pipeline.md). ABI details are in
[docs/abi.md](docs/abi.md).

The backend is one shared library:

```text
/usr/lib/onekvm/video-backends/nanokvm-mmf.so
```

`onekvm-device-nanokvm` installs it. OneKVM loads it at runtime. The CMake
project is standalone and does not use MaixCDK.

## Resolution handling

Input detection and stream output are independent. LT6911 input is not a mode
whitelist: even dimensions from 320×200 through 2880×1620 are accepted when
they stay under 5 MP and the 150M pixel/s budget. A 4K timing is recognized
but always reports `out_of_range`.

Selectable output profiles:

```text
2880x1620  2560x1440  1920x1080  1600x900  1440x1080  1440x900
1280x1024  1280x960  1280x800   1280x720
1152x864   1024x768  800x600    640x480
```

The ABI table advertised to Core is the common subset
2880×1620@30, 2560×1440@30, 1920×1080@60, 1280×720@60, 640×480@60.

SG2002 capture always uses VPSS phy channel 1 (`sc_v1`, max width 2880).
Channel 0 (`sc_d`) maxes out at 1920; a 2560-wide 1:1 convert there is tiled
and the NV21 is empty. Auto mode follows an input that can be emitted
directly. Explicit 2880×1620 is the largest native 16:9 profile. Inputs above
2560×1440 use two UYVY VI blocks; the private VPSS pool stays at three blocks,
which fits the NanoKVM 64 MiB video carveout. That profile is capped at 30 FPS
by the pixel budget. 720p can run at 120 FPS when the HDMI source emits
1280×720@120.

## Driver stack

Userspace MMF libraries are built from official Sophgo sources, pinned as one
2026-06-30 set. Prebuilt Sipeed MMF blobs are not used.

| Part | Source | Pin |
| --- | --- | --- |
| Video userspace (`cvi_mpi`) | [`sophgo/cvi_mpi`](https://github.com/sophgo/cvi_mpi) `sg200x-dev` | `75c181ee6e25baca9729a4a9b415f36180b54f93` |
| Sensor list (LT6911) | [`sophgo/SensorSupportList`](https://github.com/sophgo/SensorSupportList) `sg200x-dev` | `f064b02ba8a82746f3e87a2c5bb3bd683ff95db0` |
| `cvi_mpi` build-time osdrv | [`sophgo/osdrv`](https://github.com/sophgo/osdrv) `sg200x-dev` | `aa542c41df94f7bc656cb740f6622a5dca7dc403` |
| Video codec firmware | [`sophgo/ramdisk`](https://github.com/sophgo/ramdisk) | `1ec8fcb63a358c17c369bac38eb42dc16f30a3bb` |
| Video kernel modules | workspace `osdrv-sg200x` (Sophgo osdrv plus NanoKVM patches) | recipe `onekvm-device-nanokvm-mmf-modules` |

Kernel modules include `soph_vcodec.ko`, `soph_jpeg.ko`, `soph_vi.ko`, and
`soph_vpss.ko`. Bind-thread teardown and 5.15/6.18 compatibility live in that
module recipe, not in this repository. The running Linux image is selected by
`onekvm-distro` (`linux-sophgo`); this repo does not pin a kernel.

Do not mix a different `cvi_mpi`, osdrv, or firmware revision with this
backend. The three IPKs that must be installed together are:

- `onekvm-device-nanokvm-mmf-runtime` — `cvi_mpi` libraries
- `onekvm-device-nanokvm-mmf-modules` — `soph_*` kernel modules
- `onekvm-device-nanokvm` — this backend `.so`

## Host-side tests

No SG2002 SDK and no device:

```sh
cmake -S . -B build/host \
  -DONEKVM_BUILD_MMF_BACKEND=OFF \
  -DBUILD_TESTING=ON \
  -DCMAKE_BUILD_TYPE=Release
cmake --build build/host --parallel
ctest --test-dir build/host --output-on-failure
```

These cover resolution policy, output FPS clamping, VI FPS parsing, H.264
annex B, hardware-latency proc parsing, EDID board probe, snapshot admission,
and no-signal frames.

## Building for the device

From `onekvm-distro`, with machine `onekvm-nanokvm`:

```sh
make package mmf
```

That builds `onekvm-device-nanokvm-mmf-runtime` and `onekvm-device-nanokvm`.
Equivalent shell:

```sh
./scripts/kas.sh shell kas/nanokvm-sd.yml \
  -c 'bitbake -c package_write_ipk onekvm-device-nanokvm-mmf-runtime onekvm-device-nanokvm'
```

To compile the current working tree without changing recipe `SRCREV`:

```sh
ONEKVM_DEBUG_WORKTREES=onekvm-nanokvm-mmf \
  ./scripts/kas.sh shell kas/nanokvm-sd.yml \
  -c 'bitbake -c package_write_ipk onekvm-device-nanokvm'
```

See `onekvm-distro/docs/debug-build.md`. Deploy the application IPK only, then
restart `onekvm.service`. Do not change U-Boot, the kernel, or partitions
unless that work is explicitly authorized.

Device-side smoke tests (`tests/bound-reader-stress.cpp`,
`tests/managed-snapshot-smoke.cpp`) are compiled only when both
`ONEKVM_BUILD_MMF_BACKEND=ON` and `BUILD_TESTING=ON`. The OE recipe hard-codes
`-DBUILD_TESTING=OFF`; a debug worktree does not change that. Override
`EXTRA_OECMAKE` if you need those binaries.

`tools/lt6911-resolution-probe.c` is not a CMake target. The installed
`.so` hides every symbol except the two query entry points, so the probe
cannot `dlsym` LT6911 helpers from the production library. Use watcher
logs and I2C from a debug build instead.

## OneKVM ABI

The shared library exports exactly two entry points:

- `onekvm_video_backend_query`
- `onekvm_crypto_backend_query`

Interfaces are in `include/onekvm/video_backend_v1.h` and
`include/onekvm/crypto_backend_v1.h`. Both are versioned. Hardware crypto is
not advertised for concurrent H.265 sessions; Core uses software AES-GCM
there. First CryptoDMA `ETIMEDOUT` disables offload for the rest of the
process.

## No-signal assets

Source PNGs are under `assets/no-signal/`. After changing them:

```sh
go run ./tools/pack_no_signal.go src/no_signal_frames.inc
```

The runtime does not parse PNG. Bound no-signal stills go through VPSS as
NV21 (`render_no_signal_nv21` → `submit_vpss_nv21`); WAVE4 encodes them on
the normal bind path. `src/no_signal_h264.inc` is only used by host tests.

## License

GNU General Public License v3.0. See [LICENSE](LICENSE).
