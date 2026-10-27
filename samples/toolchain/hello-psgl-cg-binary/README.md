# hello-psgl-cg-binary

Shaders are compiled offline and loaded from their binary containers at run
time, the way an application ships them. `rsx-cg-compiler` builds
`shaders/tintvp.vcg` and `shaders/tintfp.fcg` into raw `.vpo`/`.fpo`
containers, and the build embeds them.

- Left triangle (green): both programs come from the embedded buffers through
  `cgCreateProgram(ctx, CG_BINARY, buffer, profile, NULL, NULL)`. The
  container is copied, so the buffer may be freed afterwards.
- Right triangle (orange): the same containers are written to new private
  files in `/dev_hdd0/tmp` (never over an existing file; removed at exit)
  and loaded through
  `cgCreateProgramFromFile(ctx, CG_OBJECT, path, profile, NULL, NULL)`.

`cgCreateProgram` takes no length, so the buffer must hold a complete
container that matches its own header.

Program creation and loading are checked with `cgGetLastErrorString`, which
prints the error text and clears it, and every frame checks both the GL and
the Cg error state, so a failed draw on any frame ends the sample with
`fail` and a nonzero status. The sample also requires that Cg source is not
compiled on the console: `cgCreateProgram(..., CG_SOURCE, ...)` must return
NULL with `CG_INVALID_ENUMERANT_ERROR`.

The sample uses the raw container format on purpose. Programs can also be
created from compact CGB containers, but rendering with a compact fragment
program currently draws nothing (a known defect, tracked separately), so
this sample does not use them.

It prints `hello-psgl-cg-binary: ready`, then `done` after 900 frames (or
`quit` when Quit is chosen from the XMB).

Requires an SDK newer than v0.26.0: in v0.26.0, `cgCreateProgram` returns
NULL for every input and `cgCreateProgramFromFile` refuses `CG_OBJECT`. The
next release ships the working runtime.
