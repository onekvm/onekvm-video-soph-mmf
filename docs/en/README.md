# Documentation

English | [简体中文](../zh/README.md)

For agents and anyone changing video. Session logs stay in
`../../local-docs/` (gitignored). Fixture results live in the workspace
`onekvm/docs/` directory; do not copy rXX notes back here.

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

## Related workspace notes

These files are not in this repository. They are the source for measured
results:

| Document | Topic |
|----------|-------|
| `docs/linux-6.18.md` | Current default kernel bring-up |
| `docs/2k-30.md` | 2560×1440@30 |
| `docs/3k-30.md` | 2880×1620@30 |
| `docs/720p-high-refresh.md` | 1280×720@120 |
| `docs/cryptodma-srtp-timeout.md` | CryptoDMA completion bit |
| `docs/managed-snapshot-validation.md` | Managed snapshots |
| `docs/linux-5.15.md` | 5.15 experiments (not the default) |

Build the MMF runtime, backend, and IPKs with `onekvm-distro`. Do not
cross-compile this tree outside that distro.
