# hello-psgl-cg-binary

Shaders are compiled offline and loaded from their binary containers at run
time, the way an application ships them. `rsx-cg-compiler` builds
`shaders/tintvp.vcg` and `shaders/tintfp.fcg` into raw `.vpo`/`.fpo`
containers, and the build embeds them.

- Left triangle (green): both programs come from the embedded buffers through
  `cgCreateProgram(ctx, CG_BINARY, buffer, profile, NULL, NULL)`. The
  container is copied, so the buffer may be freed afterwards.
- Right triangle (orange): the same containers are written to
  `/dev_hdd0/tmp` and loaded through
  `cgCreateProgramFromFile(ctx, CG_OBJECT, path, profile, NULL, NULL)`.

`cgCreateProgram` takes no length, so the buffer must hold a complete
container that matches its own header.

Every Cg step is checked with `cgGetLastErrorString`, which prints the error
text and clears it. The sample also shows that Cg source is not compiled on
the console: `cgCreateProgram(..., CG_SOURCE, ...)` returns NULL with
`CG_INVALID_ENUMERANT_ERROR`.

The sample uses the raw container format on purpose. Programs can also be
created from compact CGB containers, but rendering with a compact fragment
program currently draws nothing (a known defect, tracked separately), so
this sample does not use them.

It prints `hello-psgl-cg-binary: ready`, then `done` after 900 frames (or
`quit` when Quit is chosen from the XMB).
