# onekvm-nanokvm-mmf

English | [简体中文](README.zh-CN.md)

`onekvm-nanokvm-mmf` provides the NanoKVM video and crypto hardware
backend for OneKVM. It integrates the SG2002 multimedia pipeline with the
OneKVM backend ABI while keeping device-specific implementation details out of
`onekvm-server`.

## Features

- Captures video from the LT6911 HDMI input.
- Encodes H.264, H.265, and MJPEG in hardware.
- Recognizes common HDMI input modes from 640x480 through 2560x1440.
- Provides 1080p, 720p, and 480p OneKVM output profiles. 1080p is up to 60 FPS;
  720p can follow a 120 Hz HDMI source.
- Reports the HDMI signal state and provides a built-in no-signal frame to
  callers using the raw-frame path.
- Uses the official CVITEK SPACC driver for optional AES-GCM acceleration.

The Cube HDMI capture path, CSI bridge matching, HDMI watcher, and VENC
reader rules are documented in [docs/video-pipeline.md](docs/video-pipeline.md).

The backend is deployed as a single shared library:

```text
/usr/lib/onekvm/video-backends/nanokvm-mmf.so
```

The `onekvm-device-nanokvm` package installs this library, and OneKVM loads it
at runtime when needed. The project uses a standalone CMake build and does not
depend on MaixCDK.

## Resolution handling

Input detection and stream output are handled independently. The LT6911 input
path currently recognizes the following 12 HDMI resolutions:

```text
2560x1440  1920x1080  1600x900  1440x1080  1440x900
1280x1024  1280x960  1280x800   1280x720
1152x864   1024x768  800x600    640x480
```

The SG2002 VPSS pipeline scales or crops the input. Auto mode follows a
supported HDMI input, including 2560x1440@30. Explicit output can still be
1920x1080, 1280x720, or 640x480. 1440p is up to 30 FPS, 1080p up to 60 FPS,
and 720p can run at 120 FPS when the HDMI source emits 1280x720@120.

## Driver sources and versions

Release builds compile the driver stack and userspace libraries from the
official Sophgo sources. Prebuilt MMF components from the Sipeed SDK are not
used.

All parts are pinned to the same 2026-06-30 driver set:

| Part | Official repository | Version used by OneKVM |
| --- | --- | --- |
| Linux kernel | [`sophgo/linux_5.10`](https://github.com/sophgo/linux_5.10) | `sg200x-dev`, commit `767d3c5ab10b066d2d5c7c0bd1eab8a5340e923d` |
| Video kernel drivers | [`sophgo/osdrv`](https://github.com/sophgo/osdrv) | `sg200x-dev`, commit `aa542c41df94f7bc656cb740f6622a5dca7dc403` |
| Video userspace libraries | [`sophgo/cvi_mpi`](https://github.com/sophgo/cvi_mpi) | `sg200x-dev` weekly, commit `75c181ee6e25baca9729a4a9b415f36180b54f93` |
| LT6911 support | [`sophgo/SensorSupportList`](https://github.com/sophgo/SensorSupportList) | `sg200x-dev`, commit `f064b02ba8a82746f3e87a2c5bb3bd683ff95db0` |
| Video codec firmware | [`sophgo/ramdisk`](https://github.com/sophgo/ramdisk) | commit `1ec8fcb63a358c17c369bac38eb42dc16f30a3bb` |

OSDRV provides the SG2002 video kernel modules, including
`soph_vcodec.ko`, `soph_jpeg.ko`, `soph_vi.ko`, and `soph_vpss.ko`.

OneKVM adds a small set of NanoKVM compatibility and bug-fix patches. The
video driver itself still comes from the official Sophgo OSDRV source.

These revisions form a matched driver set. Combining a different kernel,
OSDRV, or `cvi_mpi` revision may prevent the video stack from starting.

## Host-side tests

The basic tests do not need an SG2002 SDK or a NanoKVM device:

```sh
cmake -S . -B build/host \
  -DONEKVM_BUILD_MMF_BACKEND=OFF \
  -DBUILD_TESTING=ON \
  -DCMAKE_BUILD_TYPE=Release
cmake --build build/host --parallel
ctest --test-dir build/host --output-on-failure
```

They check resolution changes, VI frame-rate parsing, and no-signal frame
generation.

## Cross-compiling for NanoKVM

You need an SG2002 RISC-V toolchain and a built copy of the pinned
`sophgo/cvi_mpi` source:

```sh
cmake -S . -B build/sg2002 \
  -DCMAKE_TOOLCHAIN_FILE=/path/to/sg2002-toolchain.cmake \
  -DCMAKE_OBJCOPY=/path/to/riscv64-unknown-linux-musl-objcopy \
  -DCVI_MPI_ROOT=/path/to/built/cvi_mpi \
  -DCMAKE_BUILD_TYPE=Release \
  -DBUILD_TESTING=OFF
cmake --build build/sg2002 --parallel
DESTDIR="$PWD/stage" cmake --install build/sg2002 --prefix /usr
```

The vendor SG2002 tune flags are enabled by default. Toolchain files that
already define the target ISA, such as OpenEmbedded toolchains, should add
`-DONEKVM_USE_VENDOR_TUNE_FLAGS=OFF`. Installation includes both the backend
library and its OneKVM device/system-plugin descriptors.

For reproducible release artifacts, use
`onekvm-distro/scripts/oe-nanokvm-mmf-artifact.sh`. The script checks out the
locked driver revisions, builds `cvi_mpi`, and compiles the backend with the
matching toolchain.

## On-device verification

- `tests/video-backend-abi-smoke.cpp` loads the installed backend and captures
  and encodes a frame.
- `tests/video-backend-benchmark.cpp` measures capture FPS, encoding FPS,
  bitrate, and encoding time.
- `tools/lt6911-resolution-probe.c` helps diagnose HDMI resolution detection.

## OneKVM ABI

The shared library exports exactly two entry points:

- `onekvm_video_backend_query` for video capture and encoding.
- `onekvm_crypto_backend_query` for optional AES-GCM acceleration.

Their interfaces are defined in `include/onekvm/video_backend_v1.h` and
`include/onekvm/crypto_backend_v1.h`. Both have explicit versions, so OneKVM
can reject an incompatible backend instead of crashing. MMF functions and
vendor functions stay private inside the shared library.

## Video pipeline

For a normal H.264/H.265 stream, the capture hardware feeds the encoder
directly. Full 1080p frames do not pass through OneKVM Core. MJPEG also sends
the captured frame straight to the hardware JPEG encoder.

Only the much smaller encoded result is copied into a reusable output buffer
before it is handed to Core. If another feature needs access to raw frames at
the same time, the backend switches to a general path that may copy one raw
frame. This is an internal fallback, not a second public API.

AES-GCM offload goes through OneKVM's small adapter to the official
`cvitek_spacc` kernel driver. Hardware crypto is currently not advertised for
concurrent H.265 sessions because that driver combination can lock the SG2002;
OneKVM Core falls back to its normal software AES-GCM path in that case.

## No-signal assets

The source images are under `assets/no-signal/`. After changing them,
regenerate the NV21 data with:

```sh
go run ./tools/pack_no_signal.go src/no_signal_frames.inc
```

## License

This project is licensed under the GNU General Public License v3.0. See
[LICENSE](LICENSE).
