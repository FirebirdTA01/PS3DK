# cellGcm program-handle path — status & sample regression (2026-04-26)

## Path is wired end-to-end

**Headers** (`stage/ps3dev/ps3dk/ppu/include/cell/gcm/`):
- `gcm_cg_bridge.h` — inline emitters for `cellGcmSetVertexProgram`, `cellGcmSetFragmentProgram`, `cellGcmSetVertexProgramParameter` (overloaded for both `(ctx, prog, ucode)` and PSL1GHT-style `(prog, ucode)` signatures).
- `gcm_cg_func.h` — declarations for `cellGcmCgInitProgram`, `cellGcmCgGetUCode`, `cellGcmCgGetNamedParameter`, plus the full parameter-introspection family.

**Implementations** (`sdk/libgcm_cmd/src/gcm_cg.c`, exported from `libgcm_cmd.a`):
- All `cellGcmCg*` helpers walk the `CgBinaryProgram` layout from `<Cg/cgBinary.h>` directly.
- No hidden runtime state; each call is an offset-to-pointer / struct-field read.

**Linker integration:** add `-lgcm_cmd` to the link line. The canonical samples
already do this in their `target_link_libraries(...)` clauses (e.g.
`hello-ppu-cellgcm-clear/CMakeLists.txt:18`).

## Compatible blob format

`cellGcmCg*` consumes only the **CgBinaryProgram** layout:
```
+0x00 profile          (u32)
+0x04 revision         (u32)
+0x08 totalSize        (u32)
+0x0c parameterCount   (u32)
+0x10 parameterArray   (CgBinaryOffset)
+0x14 program          (CgBinaryOffset)
+0x18 ucodeSize        (u32)
+0x1c ucode            (CgBinaryOffset)
```
This is what **sce-cgc** emits. It is also what **rsx-cg-compiler** emits for
the supported shader subset (Phase 8 stage 4 — 46/46 byte-match).

Engine workflow that lights up this path today:
1. Compile shaders with sce-cgc (works under wine; confirmed by the engine
   session at `/home/firebirdta01/cell-sdk/475.001/cell/host-win32/Cg/bin/sce-cgc.exe`).
2. `bin2s` the resulting `.vpo` / `.fpo` into the binary.
3. Cast the embedded array to `CGprogram` and call `cellGcmCgInitProgram`,
   `cellGcmCgGetUCode`, `cellGcmCgGetNamedParameter`.
4. Bind via `cellGcmSetVertexProgram(ctx, prog, ucode)` and
   `cellGcmSetFragmentProgram(ctx, prog, fp_offset)`.
5. Push uniforms via `cellGcmSetVertexProgramParameter(ctx, param, values)`.

## In-tree sample regression — out of scope for this step but worth tracking

`samples/gcm/hello-ppu-cellgcm-{loops,chain,laneelision}` crash at startup with
a VM access violation reading `0x106d7264` (or similar address inside or past
`.rodata`).

Root cause: those samples are built with `ps3_add_cg_shader(...)` which invokes
**cgcomp** (PSL1GHT). cgcomp emits a different blob layout (`"VP\0\0"` magic at
offset 0). When the sample casts that blob to `CGprogram` and calls
`cellGcmCgGetUCode`, the helper reads the cgcomp blob's bytes as CgBinaryProgram
fields. Field offsets don't line up — `prog->ucode` resolves to a multi-megabyte
offset that lands beyond `.rodata`, and the next dereference faults.

This is **not** a bug in `cellGcmCg*` itself. It's a format mismatch between the
sample's compile path (cgcomp) and the runtime API the sample chose
(`cellGcmCg*`). Fix is one of:

1. Switch `ps3_add_cg_shader()` to use rsx-cg-compiler (now byte-match against
   sce-cgc for the subset these samples use). Requires a build-system change in
   `cmake/ps3-self.cmake:ps3_add_cg_shader` to call `rsx-cg-compiler` instead of
   `cgcomp`.
2. Or switch loops/chain/laneelision to call PSL1GHT's `rsxLoadVertexProgram` /
   `rsxLoadFragmentProgramLocation` path (which understands cgcomp output) like
   `hello-ppu-cellgcm-triangle` does. Two files per sample.

Option 1 is the cleaner long-term fix and aligns with the SDK direction memory
record (PSL1GHT becoming a back-compat shim over the cell/* surface).

## Implication for the engine session

The cellGcmSetVertexProgram + cellGcmCg* path is ready to consume sce-cgc
output. The engine can drop reliance on rsxLoad* + cgcomp entirely for the
shaders that need features cgcomp / our compiler don't yet support
(`discard`/KIL, complex CFG, etc.). Add `-lgcm_cmd` to the link line, swap
the API call sites, point the build at sce-cgc for those shaders.
