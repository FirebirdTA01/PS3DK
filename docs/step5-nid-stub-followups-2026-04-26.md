# Step 5 — NID/stub follow-ups from coverage report (2026-04-26)

## Headline

**There are no missing-NID or missing-stub gaps blocking the EMP engine.** The coverage report (`docs/emp-engine-coverage-2026-04-26.md`) confirmed 218 imports, 100% NID DB coverage, all 6 imported libs supported.

## Follow-ups surfaced (none critical)

### 1. Sony-name aliases for libsysutil callback family

`libsysutil.a` exports `__sysUtilCheckCallback` / `__sysUtilRegisterCallback` (PSL1GHT names) but not the Sony names directly. Today the Sony surface is bridged via static inline forwarders in `<cell/sysutil.h>` (lines 76-86) — that works for any TU that includes the header, which is the right answer for the engine.

Lower priority: re-emit libsysutil through the nidgen archive flow (Phase 6.5 pattern, established by libsysutil_screenshot) so the Sony names land as first-class archive symbols and the PSL1GHT-name forwarders become alias entries.

### 2. Sample blob-format mismatch (loops / chain / laneelision)

Surfaced during step 4 verification, not step 1: `ps3_add_cg_shader()` builds shaders with cgcomp (PSL1GHT format), but loops/chain/laneelision feed those blobs to `cellGcmCgInitProgram` which expects CgBinaryProgram. Result: VM access violation at startup. See `docs/cellgcm-program-handle-status-2026-04-26.md` for the full analysis.

Fix paths (pick one when revisited):
- Switch `ps3_add_cg_shader()` to invoke rsx-cg-compiler instead of cgcomp.
- Switch the three samples back to `rsxLoadVertexProgram` / `rsxLoadFragmentProgramLocation` (the path triangle uses) since they're cgcomp-format-only.

### 3. DRAWING_BEGIN/END pause-and-skip pattern

Triangle now models the canonical handler: `g_drawing_paused` toggles on `CELL_SYSUTIL_DRAWING_BEGIN` / `SYSTEM_MENU_OPEN`, render loop skips emission with a vblank-sized usleep while paused. Loops/chain were retrofitted with the same pattern but the per-frame VP-ucode upload they do interacts badly with the skip path (samples already broken from issue #2 above; the retrofit was reverted).

Proper fix when revisiting: `cellGcmFlush(ctx)` + `cellGcmSetWaitFlip(ctx)` on the DRAWING_BEGIN edge to drain the FIFO before yielding, then resume from a quiesced state on DRAWING_END. Don't just skip; explicitly hand off.

## Done checklist

- [x] Coverage report shows 100% NID resolution across 6 imported libs.
- [x] Engine cannot be unblocked by adding more NIDs/stubs — its issue is engine-side per-Entity render-path sequencing (per the step-2 setDrawEnv repro proving toolchain is clean).
- [x] cellGcm program-handle path (step 4) is wired and consumes sce-cgc output.
- [x] sysutil callback dispatch (step 3) is wired; sysmenu sample verifies it; triangle has the canonical DRAWING_BEGIN/END handler pattern.

## What the engine session needs from us next

Nothing toolchain-side. Their next move is one of:

1. Compile shaders with sce-cgc, switch their renderer to call
   `cellGcmSetVertexProgram` / `cellGcmSetFragmentProgram` /
   `cellGcmCgGetNamedParameter` / `cellGcmSetVertexProgramParameter`
   (link `-lgcm_cmd`). Removes the rsxLoad* path from their code entirely.
2. If they prefer to stay on `rsxLoad*`, isolate their per-Entity VP-ucode
   upload — most likely culprit per the bisection (desync command
   `0x401F9C6C` was their VP instruction-2 first word, strong signal the
   upload is misaligned with the FIFO read pointer).
