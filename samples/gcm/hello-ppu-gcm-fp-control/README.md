# Fragment-program control

Compiles the existing triangle sample's color-pass-through fragment shader,
loads its `CGprogram`, and emits `cellGcmSetFragmentProgramControl` into a
private two-word command buffer. The preallocated Unsafe form must emit the
same words. The sample checks the command header, exact word count, and guards;
it prints the shader metadata and control word for inspection.

Build with the SDK environment set:

```sh
cmake -S . -B build -DCMAKE_TOOLCHAIN_FILE="$PS3DK/cmake/ps3-ppu-toolchain.cmake"
cmake --build build
```

For a source checkout, use the repository's `cmake/ps3-ppu-toolchain.cmake`.
Keep the sibling triangle sample present: it supplies the shader. Run the
SELF and read its console output. Success returns 0; a mismatch returns 1.

There is no GCM initialization, display loop, or GPU submission. A PASS checks
emission consistency, not shader execution. `controlTxp` and both reserved
arguments are zero; the unresolved meaning of control bit 15 is not tested.
For rendering and program installation, see `hello-ppu-cellgcm-triangle`.
