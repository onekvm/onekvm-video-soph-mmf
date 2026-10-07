# OpenEmbedded layer

This layer supports the `wrynose` series and depends on OE-Core and
`onekvm-bsp` from `onekvm-distro`. It provides the NanoKVM backend recipe
`onekvm-video-soph-mmf` and the vendor userspace recipe
`onekvm-video-soph-mmf-runtime`, including its build compatibility patches.
The kernel module and codec firmware recipes remain in the distro BSP layer.

Build through `onekvm-distro` with `make package mmf`. Its
`kas/components.lock` selects this layer and supplies
`ONEKVM_COMPONENT_SRCREV` for the backend source. The runtime keeps its own
fixed Sophgo MPI, sensor and osdrv revisions; it does not fetch this repository.

The initial migration also retains the old backend package version via `PKGV`.
When releasing newer backend code, advance `PV` and remove that override.

`ONEKVM_COMPONENT_LOCAL_MIRROR=1` uses local Git repositories while retaining
the locked revisions. `ONEKVM_DEBUG_WORKTREES=onekvm-video-soph-mmf` loads
this working tree's layer and overrides only the backend source and debug PR.
Runtime recipe and patch changes are read from that layer, while its vendor
sources still use the fixed revisions and normal fetch/unpack/patch tasks.
