# hello-ppu-gcm-main-arena

This sample draws a textured, indexed quad. Its vertices, 16-bit indices and texture all sit in a 1 MiB arena
mapped with `cellGcmMapMainMemory` (`CELL_GCM_LOCATION_MAIN`). The PPU rewrites the arena between phases:

1. colour A;
2. the texture is rewritten to colour B;
3. the vertices are rewritten so that the quad covers only the left half of the readback window.

**Checking.** Each phase runs 60 frames. Every frame, a 128x128 window at the screen centre is copied back to the
arena by an RSX image transfer and checked. The frame completes on application label 100, which the PPU polls with
`cellGcmIsLabelPassed`.

**Warm-up.** Unchecked frames run first, until one frame reads back correctly, because an emulator that compiles
shaders asynchronously skips the first draws.

**Output.** The output is a series of `GCM_ARENA_*` lines. A pass ends with `GCM_ARENA_OK frames=180`; a failure
prints `GCM_ARENA_FAIL <reason>` and exits 1.

The alignment and PPU write-ordering rules, and which of them are verified, are listed in
[docs/gcm-labels.md](../../../docs/gcm-labels.md#4-drawing-from-a-main-memory-arena-a3).
