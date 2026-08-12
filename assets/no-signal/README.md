# OneKVM no-signal artwork

These PNG files are deterministic screenshots of the approved Vue artwork in
`onekvm-ui/src/components/NoSignal.vue`. They are captured at each resolution
advertised by the NanoKVM MMF source and converted to packed NV21 arrays by:

```sh
go run ./tools/pack_no_signal.go src/no_signal_frames.inc
```

The runtime does not parse PNG files. `libkvm_mmf.so` expands the generated
read-only arrays directly into the caller's NV21 frame buffer.
