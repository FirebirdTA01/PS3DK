# Handoff: align PSL1GHT `setTransferImage` to the reference SDK (blitting sample)

**For:** a fresh session running on the CachyOS dual-boot (where the proprietary reference Sony SDK 475.001 is mounted).
**Why this exists:** the Windows session that produced this could not read the reference SDK. The reference is the oracle we must match. This brief is self-contained — do not assume prior conversation context.

---

## 0. One-line goal
Make our PSL1GHT librsx `rsxSetTransferImage` (and the scale-transfer path) emit the **same RSX command stream the reference SDK's `cellGcmSetTransferImage` emits**, so the PSL1GHT `blitting` sample renders fully and runs continuously under RPCS3 (which runs Sony's real library). Two residual issues remain; both trace to our emission not matching the reference.

## 1. What blitting is / where it lives
- Sample: `samples/PSL1GHT/graphics/blitting/` — `source/main.cpp`, `source/rsxutil.cpp`. Byte-identical to upstream PSL1GHT `5ba78ae`.
- It draws (per frame): a clear, the full PSL1GHT logo (left), a **per-row "distort" effect** at `display_width*3/4` (top-right) using **height=1** blits, animated letters (`blit_simple`), and zoomed letters (`blit_scale`), then `flip()`.
- librsx it links: built+installed from `src/ps3dev/PSL1GHT/ppu/librsx/commands_impl.h` → `$PS3DK/ppu/lib/librsx.a`. (`sdk/librsx/src/commands_impl.h` is an identical copy; both are post-f9992c9.)

## 2. Blocking bugs already fixed (blitting now RENDERS)
1. **srcX fix — committed `3a21a24`** (branch `feature/psl1ght-samples-compat`). `blit_simple` passed `srcX/srcY` through `rsxGetFixedUint16()` (fixed-point), but post-f9992c9 `rsxSetTransferImage` uses them as **integer** pixel offsets. Changed to plain `srcX/srcY`.
2. **SRCCOPY fix — UNCOMMITTED in the working tree** (`blit_scale`, main.cpp). `scale.operation` was `GCM_TRANSFER_OPERATION_SRCCOPY_AND` (=0); RPCS3's NV3089 `decode_transfer_registers` implements ONLY `srccopy` (=3) and calls `recover_fifo()` → Dead FIFO on anything else. Changed to `GCM_TRANSFER_OPERATION_SRCCOPY`. **Director approved keeping it; commit it** (verify against the reference's `cellGcmSetTransferScaleImage` operation first — see §5).

## 3. Root cause of the residuals (the key finding)
Upstream PSL1GHT commit **`f9992c9` (2021-02-08, "properly implement setTransferImage")** changed `rsxSetTransferImage`'s srcX/srcY semantics from **FIXED-POINT → INTEGER** and added 1024-px block tiling, **but never updated the blitting sample**. So:
- **Older PSL1GHT** (pre-f9992c9, fixed-point librsx) + the fixed-point sample = match = renders on RPCS3 + hardware. **This is what the director tested as "works."**
- **Our SDK** (post-f9992c9 integer librsx) + the unchanged sample = mismatch → the original Dead-FIFO and the residuals.

Emission difference (from `git show f9992c9 -- ppu/librsx/commands_impl.h` in the upstream clone):
- OLD: `in_size = (height + ((srcY+15)>>4))<<16 | (width + ((srcX+15)>>4))`; `IMAGE_IN point = (srcY<<16)|srcX` (fixed-point); single blit, no tiling.
- NEW (ours): `srcBlockOffset = bpp*(srcX + x - dstX)`; `IMAGE_IN point = 0`; tiled into ≤1024 blocks.

## 4. Director's directive (the framing)
"Make it closely match the **reference SDK and its API**." Resolution so far:
- Sony's `cellGcmSetTransferImage` takes **integer** srcX/srcY → our integer librsx + the integer sample fix match the **API**. Direction = keep integer; do NOT revert to fixed-point.
- BUT matching the API is not enough — the **emitted RSX stream** must match too. RPCS3 (Sony's real lib) renders older-PSL1GHT's height=1 transfer but NOPs ours, so PSL1GHT's f9992c9 rewrite is **not reference-accurate**. The fix is to align our emission to the reference's, NOT to revert.

## 5. THE TASK ON CACHYOS — read the reference and align
Read the reference SDK (Sony 475.001; mounted on CachyOS — policy: `reference/REFERENCE_POLICY.md`, never copy code into the repo, oracle-only).

**(A) `cellGcmSetTransferImage`** — find its implementation (cell/gcm; likely an inline in `cell/gcm/gcm_method*.h` or libgcm source). Determine the exact RSX methods/values it emits for an image transfer, especially:
- Does it use the **pure NV3089 / NV04_SCALED_IMAGE_FROM_MEMORY** path — i.e. does it set `NV3089 SET_CLIP_RECTANGLE` (blit_engine_clip) and the **NV3089 IMAGE_OUT** (blit_engine_output) registers — rather than PSL1GHT's **NV01_IMAGE_FROM_CPU** hybrid? RPCS3's `decode_transfer_registers` reads `blit_engine_output_width/height` and `blit_engine_clip_width/height`; our NV01-hybrid emission apparently leaves those wrong for height=1, so RPCS3 computes `clip_w/clip_h == 0` and NOPs ("empty regions").
- How does it handle a **height=1** transfer specifically?
Then **modify `src/ps3dev/PSL1GHT/ppu/librsx/commands_impl.h` `RSX_FUNC(SetTransferImage)`** (independently re-implemented, not copied) to emit the reference-matching stream. Keep `sdk/librsx/src/commands_impl.h` in sync.

**(B) `cellGcmSetTransferScaleImage`** — compare to `rsxSetTransferScaleSurface`; confirm the correct raster operation (we changed SRCCOPY_AND→SRCCOPY for RPCS3 — verify the reference uses plain srccopy for an opaque/alpha-blended copy).

**(C) Default command buffer / flush — for the hang (§6).** Read `cellGcmSetDefaultCommandBuffer`, the default command-buffer **callback** (the wrap/flush handler), and `cellGcmFlush`/`cellGcmFinish`. Compare to our `rsxInit`/`gcmInitBodyEx`/`rsxFlushBuffer` and the wrap-callback path (`rsx_function_macros.h` `rsxContextCallback`, invoked via compact-OPD `__get_opd32`).

## 6. Residual details (verify after each change in RPCS3)
**#1 Empty top-right (distort):** the per-row loop (main.cpp ~line 172) emits `bitmap.height` blits of **height=1**. RPCS3 logs `NV3089_IMAGE_IN: Operation NOPed out due to empty regions` and drops them. Our FIFO is byte-correct for the post-f9992c9 logic (verified by dumping `context->current` before/after the call). **Tiling ruled out** (moved the distort off the 1024 boundary → NOP still fired) — it's the emission semantics / which registers get set. Fix via §5(A).

**#2 ~2s hang (freezes, process still alive, PS-button overlay works):** the 1MB command-buffer **ring fills at ~frame 110** (blitting's per-frame volume is high) and the wrap **deadlocks**. PROVEN: doubling `CB_SIZE` (include/rsxutil.h `0x100000`→`0x200000`) doubled the freeze frame 100→220 (director independently saw the 2MB build run ~2× longer). **Do NOT band-aid with a bigger CB_SIZE (director was explicit).** Older PSL1GHT doesn't hang on the same RPCS3 → our wrap/flush/throttle differs. Suspect the flip-throttle (`rsxutil.cpp` `flip()`/`waitflip()` poll `gcmGetFlipStatus`; if it doesn't throttle the PPU, it races ahead, fills the ring, and the wrap deadlocks with the pending `gcmSetWaitFlip`) and/or the wrap callback. Fix via §5(C) — find the real cause.

## 7. How to test
- Build blitting against the SDK; boot in RPCS3. Headless for logs: `rpcs3 --no-gui <self>`; then grep `log/RPCS3.log` for `Dead FIFO`, `NOPed out`, `recover_fifo`. Visual check needs a GUI boot (the distort top-right + continuous animation) — director runs that.
- **FIFO-dump technique** (to verify our emission matches the reference's): in `blit_simple`/`blit_scale`, `u32 *fb = context->current;` before the rsx call, then after: `for (j=0;j<context->current-fb;j++) printf("F[%d]=0x%08x\n",j,fb[j]);` (once, via a static flag).
- RPCS3 source proxy-oracle (if needed): `raw.githubusercontent.com/RPCS3/rpcs3/master/rpcs3/Emu/RSX/NV47/HW/nv3089.cpp` — `decode_transfer_registers()` shows exactly what it validates (`clip_w/clip_h = min(blit_engine_clip_*, out_*)`, `operation != srccopy`, etc.).

## 8. State / artifacts
- Branch `feature/psl1ght-samples-compat`; srcX fix committed `3a21a24`; SRCCOPY fix uncommitted in working tree (keep + commit).
- Upstream PSL1GHT clone (for `git show f9992c9`): on Windows it was at `C:\Users\FirebirdTA01\source\repos\PSL1GHT` (HEAD `5ba78ae`); re-clone on CachyOS if needed.
- Full saga is in the project memory `project_psl1ght_sample_port.md` (if synced to CachyOS).
- Reminder: the toolchain itself is NOT at fault here — we verified our emitted FIFO is byte-correct for the (post-f9992c9) librsx logic; the issue is that PSL1GHT's logic ≠ the reference's. Fix is in librsx's emission + the sample, aligned to the reference.
