# setDrawEnv FIFO desync — toolchain repro result (2026-04-26)

**Sample:** `samples/gcm/hello-ppu-cellgcm-drawenv/`

**Question being answered:** is the FIFO desync the EMP engine hits with `setDrawEnv` enabled reproducible from toolchain code, or only from engine-side sequencing?

## Result

**Toolchain side runs clean.** 360 frames, 0 FIFO errors, full clean exit.

The sample mirrors the engine's `PS3_renderer.cpp:setRenderTarget` + `setDrawEnv` verbatim and inserts them into the canonical clear-and-flip loop:

```
wait_flip
set_render_target(buffers[cur])      // cellGcmSetSurface
set_draw_env()                        // ColorMask, Viewport, Scissor,
                                      // DepthTest, DepthFunc, DepthMask,
                                      // ShadeModel, FrontFace, CullFace,
                                      // BlendEnable
cellGcmSetClearColor + ClearSurface
do_flip(buffers[cur].id)
```

This passes RPCS3 cleanly across all 360 frames, cycling six colours.

## Implication

The engine's FIFO desync at `setDrawEnv` enable is **not** caused by:
- the GCM surface command stream
- the viewport / scissor / depth / cull / blend state packets
- our depth-buffer-first allocation order
- the cellGcm forwarders themselves (`cellGcmSetColorMask`, `cellGcmSetViewport`, etc.)
- the rsxSetScissor PSL1GHT helper

The engine's desync is somewhere in the **per-Entity render path**. Most likely culprits, in order:
1. `rsxLoadVertexProgram` writing VP ucode that the GPU later reads as a method header (the engine's bisection found the desync command 0x401F9C6C is the first word of VP instruction 2 — strong signal that VP ucode upload is mis-aligned with the FIFO read pointer).
2. `rsxBindVertexArrayAttrib` against a stale or freed VBO offset (the engine re-binds attribs against the same VBO offset every frame, and the engine session noted "no rsxInvalidateVertexCache between frames" as a suspect).
3. `rsxLoadFragmentProgramLocation` with a malformed FP blob (cgcomp KIL / discard emit was already noted as broken — sce-cgc + the `cellGcmSetFragmentProgram` path is the documented escape hatch).

## Follow-up sample worth building

`hello-ppu-cellgcm-drawenv-vp` — extend this sample with the engine's exact VP-ucode upload sequence (rsxLoadVertexProgram against a small precompiled VP) but no draw call, just to isolate whether the upload alone desyncs. If it does, the bug is in the rsxLoadVertexProgram helper or in our libgcm_cmd's VP_UPLOAD_INST emit. If not, the bug is the engine's per-frame upload churn or VBO state.

That's the right next isolation step but is engine-side debugging — not toolchain work. The toolchain has cleared this layer.
