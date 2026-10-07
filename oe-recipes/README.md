# OpenEmbedded layer

This layer supports the `wrynose` series and depends on OE-Core and
`onekvm-bsp` from `onekvm-distro`. It provides the NanoKVM backend recipe
`onekvm-video-soph-mmf`, the vendor userspace recipe
`onekvm-video-soph-mmf-runtime` with its build compatibility patches, and
`onekvm-video-soph-mmf-modules` with its loaders, units, configuration and HDMI
prepare helper. The codec firmware recipe remains in the distro BSP layer.

Build through `onekvm-distro` with `make package mmf`. Its
`kas/components.lock` selects this layer and supplies
`ONEKVM_COMPONENT_SRCREV` for the backend source. The runtime keeps its own
fixed Sophgo MPI, sensor and osdrv revisions; it does not fetch this repository.
The modules recipe separately fetches the pinned `osdrv-sg200x` sources as an
embedded MMF build input. osdrv does not provide a separate component layer.

The initial migration also retains the old backend package version via `PKGV`.
When releasing newer backend code, advance `PV` and remove that override.

`ONEKVM_COMPONENT_LOCAL_MIRROR=1` uses local Git repositories while retaining
the locked revisions. `ONEKVM_DEBUG_WORKTREES=onekvm-video-soph-mmf` loads
this working tree's layer and overrides only the backend source and debug PR.
Runtime and modules recipe changes are read from that layer, while their
sources still use their own fixed revisions and normal fetch/unpack tasks.
For current driver code as well as working tree module packaging files, use
`ONEKVM_DEBUG_WORKTREES=onekvm-video-soph-mmf,osdrv-sg200x`.
