# Documentation

English | [简体中文](../zh/README.md)

For agents and anyone changing video. This tree contains only maintained
interfaces and architecture constraints. Session logs and fixture notes stay
in `../../local-docs/` (gitignored) and are not part of this index.

The default kernel is **Linux 6.18**
(`PREFERRED_VERSION_linux-sophgo = "6.18%"` on `onekvm-nanokvm`).

| Document | Purpose |
|----------|---------|
| [video-pipeline.md](video-pipeline.md) | Cube HDMI capture, bound H.264, teardown, CSIBDG, placeholder, snapshots. Read this first when changing video. |
| [abi.md](abi.md) | Video/crypto ABI, feature bits, leases, snapshots |
| [no-signal.md](no-signal.md) | No-signal NV21 assets |
| [overview.md](overview.md) | Features, resolution budget, 6.18 kernel, OE build, host tests |
| [../../AGENTS.md](../../AGENTS.md) | Agent hard constraints (`CLAUDE.md` is a symlink) |

The root [README.md](../../README.md) points at this tree.

Build the MMF runtime, backend, and IPKs with `onekvm-distro`. Do not
cross-compile this tree outside that distro.
