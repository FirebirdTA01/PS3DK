# Known issues, limitations, and planned improvements

Running list of things the SDK currently works around, defers, or does
in a less-than-ideal way — each entry pairs the observed symptom with
the planned fix so we don't lose track of them when moving on to the
next surface.

---

## Compact `.opd` emission: achieved (retired from issues list)

**Status:** complete.  GCC `-mps3-opd-compact` flag emits native 8-byte
`.opd` entries; binutils resolves `R_PPC64_TLSGD *ABS*` at link time to
write the module TOC base into offset +4. The transitional 24-byte-with-
compat-packing form (GCC stock backend + sprx-linker post-link) has been
retired; all call sites now use the native 2-word read sequence.

**Validation.** See `docs/abi/compact-opd-migration.md` for conformance
notes and test plan. The ABI spec (`docs/abi/cellos-lv2-abi-spec.md`)
and architecture overview (`docs/abi/toolchain-architecture.md`) have
been updated to reflect the achieved state.

---

## FIFO wrap drain-wait causes occasional frame flicker

**Status:** works correctly (no hangs, no desync), but drops the
occasional frame on a wrap.

**Symptom.** All ported samples that exercise the FIFO wrap path
(`spinning-cube`, the textured-quad port, every other
cellGcm-driven render with CB_SIZE around 64 KB) run indefinitely
with our native wrap callback, but exhibit an occasional mild
flicker — one dropped frame each time the command FIFO wraps. In
practice this lands roughly every ~1.25 s at 60 fps with the
sample's default `CB_SIZE = 0x10000` (64 KB) and ~800 FIFO bytes
per frame.  The failure mode is SDK-wide, not sample-specific:
any draw-loop that issues enough FIFO commands to wrap the ring
triggers the drain-wait spin.

**Where it is.** `sdk/libgcm_cmd/src/ps3tc_fifo_wrap.c` —
`ps3tc_fifo_wrap_callback`. It's a single-buffer in-place wrap:
writes the JUMP-to-begin command, sets `ctrl->put = begin_off`, then
spins on `ctrl->get != begin_off` via `sys_timer_usleep(30)` until
the GPU follows the JUMP. That spin **is the drain** — PPU is
blocked until the GPU has processed every command ahead of it. If
the wait lands mid-frame, the frame's budget blows past VSYNC and
you see a visible stutter.

**Four async-wrap attempts, all reverted.** We've tried to replace
the drain-wait with double-buffered back-end-label sync three
times.  Each attempt failed differently and each failure pointed
at a real constraint the final design has to satisfy:

1. **PUT set past the JUMP** — first attempt advanced PUT to a
   point inside the target half (so the GPU would follow the JUMP
   and keep consuming into new commands).  Symptom: cube rendered
   correctly at first, then entered a visible "rotation loop" —
   cube rotated forward, skipped backward, rotated forward again.
   Cause: PUT pointed into memory the PPU hadn't rewritten yet;
   GPU re-consumed whatever commands happened to still be sitting
   in the target half from the previous pass.  Fix direction: PUT
   after wrap must sit exactly at the JUMP target (where GET will
   catch up and idle), never past it.

2. **No PUT advance at wrap** — second attempt emitted the label
   + JUMP into ctx but left PUT untouched, assuming the next
   app-side `cellGcmFlush` would cover both the JUMP and the new
   commands in one bump.  Symptom: hard freeze before the first
   rendered frame.  Cause: by the time ctx->current advanced past
   the JUMP the wrap callback had already returned; the firmware
   at that point expected PUT to be coherent with ctx->current on
   the next flush, but our JUMP was "behind" PUT in ring order,
   so the GPU never saw it.  Fix direction: PUT has to move at
   the wrap itself, not be deferred to the caller.

3. **PUT = target_off, label-poll wait** — third attempt (closest
   to the design above).  Emit label-write + JUMP at ctx->current
   in the exiting half, set PUT = target_off (the entering
   half's begin), then poll the entering half's "I'm-done" label.
   Symptom: rendered ~17 frames, then froze mid-run; PPU still
   live (PS button worked), GPU stopped advancing GET.

4. **Retest of #3 with verified clean build/install** — after
   diagnosing that earlier "fix" tests had been contaminated by
   stale archives in `$PS3DEV/psl1ght/ppu/lib/` (the sample was
   linking against a not-yet-installed change), iteration 3 was
   rebuilt with explicit `make install` + symbol verification
   in the ELF (`g_half_a_begin` etc. present) and re-tested.
   Same freeze at the same ~17 frame mark.  This confirms the
   failure is a real design bug, not a build artefact.  Most
   likely causes, in priority order:

   - **~~Semaphore byte-swap mismatch.~~** Ruled out.  RPCS3's
     `nv4097::back_end_write_semaphore_release`
     (`rpcs3/Emu/RSX/NV47/HW/nv4097.cpp`) applies the exact same
     self-inverse permutation PSL1GHT emits:
     `val = (arg & 0xff00ff00) | ((arg & 0xff) << 16) | ((arg >> 16) & 0xff);`
     The two swaps cancel, so polling for the raw ticker is the
     correct shape.  Texture-read release writes `arg` verbatim
     — if we ever want a non-swapping path (easier to reason
     about) we can use `SEMAPHORE_TEXTUREREAD_RELEASE` instead,
     but the swap itself is not the bug.
   - **Label slot collision.**  We picked slots 253/254 on the
     reasoning that retail-app labels stay in 0..127, but the sample's
     `cellGcmFinish(n)` path and PSL1GHT's flip machinery both
     use labels in higher ranges.  If either touches 253/254
     the value flips under us.  A safer choice is to query
     `cellGcmGetLabelAddress` at install time, remember the two
     pointers, and reset them only through those pointers.
   - **GPU stops before reaching the label-write.**  Our label
     emit sits just before the JUMP, so if the GPU halts on an
     earlier command in the half (e.g. the surface setup in
     frame 0 triggers a state we don't handle) the label never
     fires and we wait forever.  The right mitigation is a
     bounded wait (fall back to drain-wait on timeout) — but
     that shouldn't be the first-line design, just a
     belt-and-suspenders backstop.

   - **First-wrap JUMP lands in unwritten half-B memory.**
     Most likely theory after ruling out the swap.  Our
     `ps3tc_fifo_wrap_install` shrinks `ctx->end` to half-A's
     limit but doesn't prime half-B with a valid "idle" command
     stream.  On the first wrap we point PUT at B_begin; GPU
     follows our JUMP there and finds `GET == PUT`, so it idles
     correctly *that time*.  But the firmware's preamble in
     half A may have included commands whose processing
     extends past what we think "A_end" means, and if GPU
     ends up reading any unwritten byte in B's reserve area
     as a method-0 command with a non-zero arg count, it can
     stall waiting for data that never comes.  Fix direction:
     pre-write a safe placeholder (JUMP-back-to-self or a
     single RETURN/NOP pair) at every half's reserve area at
     install time, so any stray read lands on a well-defined
     no-op.

**What the correct fix needs.**  In order:

1. Confirm on RPCS3 exactly how `NV4097_SET_SEMAPHORE_RELEASE`
   writes the value — swapped or un-swapped — so the PPU-side
   poll compare is the right shape.  Same check for real PS3 if
   we get hardware access later.
2. Reserve label slots through `cellGcmSetDefaultCommandBuffer` /
   `cellGcmGetLabelAddress` rather than hard-coding 253/254, and
   make sure nothing else in our stack (sysutil, flip path,
   dbgfont tier-2) touches the same slots.
3. Keep the single-buffer drain-wait as the fallback — not
   removed from the file, just behind a compile-time switch —
   so a broken async path can be reverted without a rebuild of
   the whole SDK.
4. Test against spinning-cube (tight wraps, small CB), flip_immediate
   (larger CB, flip-synced), and our own triangle sample
   simultaneously before declaring it fixed.  The failure modes
   so far have all been "works for N frames then desyncs", so
   a short validation run is not sufficient.

**Why we're not doing it right now.** Three iterations without
progress indicates the issue is in our understanding of how
SEMAPHORE_BACKENDWRITE_RELEASE lands on RPCS3 / real hardware,
not in the callback glue.  Resolving that is more about reading
the RPCS3 source / NV40 docs than writing more code.  The
current single-buffer-with-drain-wait version is correct under
all observed workloads; it just flickers on each ~1.25 s wrap.
We revisit when either the flicker blocks a sample we care
about, or we have independent verification of the semaphore
encoding on both targets.

---

## <cell/dbgfont.h> on-screen renderer — per-glyph rotation pending

**Status:** tier-2 shipped.  `sdk/libdbgfont/libdbgfont.a` draws real
text over the frame via NV40 through the native `cellGcm*` surface
(128×128 alpha atlas, passthrough VP + alpha-kill FP compiled by
cgcomp, 8-bit unit remapped to RGBA, QUADS draws).  Header is now
pure declarations; samples link `-ldbgfont`.  Verified in
`samples/gcm/hello-ppu-dbgfont` with rainbow / pulse / rigid-word-
spin animations.

**Enhancement — `cellDbgFontPutsRotated`.**  The current Printf/Puts
API emits axis-aligned quads, so callers can rotate where a *word*
sits on screen (reposition each letter along a rotating direction
vector — what the dbgfont sample's SPIN line does) but cannot tilt
the *glyphs themselves*.  The natural extension is a new entry:

```c
int32_t cellDbgFontPutsRotated(float x, float y, float scale,
                               float angle_rad,
                               uint32_t color, const char *s);
```

Implementation shape (in `sdk/libdbgfont/src/dbgfont.c`):

1. Compute `cosθ, sinθ` once per call.
2. For each glyph, compute the four quad corners relative to the
   glyph's local pivot, rotate each by `(cosθ, sinθ)`, then add the
   glyph's anchor point in screen NDC.  The quad becomes a rotated
   parallelogram instead of an axis-aligned rectangle — same UV
   assignments as today, different positions.
3. Advance the horizontal layout cursor in the word's local frame
   (rotated basis), so successive glyphs stay neighbours even when
   the whole string is tilted.
4. Factor `append_glyph` so Puts (no rotation) and PutsRotated share
   the UV / color / pool-push bookkeeping — only the four positions
   differ.

No shader changes needed — the rotation happens on CPU in the vertex
write-out, and the existing passthrough VP carries the already-rotated
positions straight to NDC.  Aspect-ratio correction for non-square
screens is the caller's responsibility (same as the word-rotation
pattern in the sample) unless we want to bake in a screen-aspect
uniform; keeping it explicit keeps the API orthogonal.

Follow-up after this: add `cellDbgFontPrintfRotated` (va_list form)
and optionally a pivot-offset variant for rotating around a glyph-
local anchor other than the baseline.

---

## SPRX stub trampolines: frame-less wrapping shape, not bctr tail-call

**Status:** fixed 2026-04-27 in `tools/nidgen/src/stubgen.rs`. Documented
here because the divergence from the reference SDK stub shape is
load-bearing and easy to regress.

**Symptom.** With a `bctr` tail-call leaf trampoline (the reference
SDK's stub shape, byte-identical to `libspurs_stub.a`), every SPRX call
that touches the TOC after returning crashes — `r2` is left holding the
SPRX's TOC instead of the caller's.  With an `stdu`-allocated wrapping
trampoline (our original shape), every SPRX function with **>8
arguments** (the spillover lives at `caller_sp+112+`) returns
`CELL_*_ERROR_INVAL` because the SP shift moves the parameter-save area
out from under the SPRX's argument loads.  The canonical reproducer is
`cellSpursJobChainAttributeInitialize` (14 args, 6 spilled).

**Why the reference shape doesn't work in our toolchain.**
The reference's `bctr` tail-call assumes the caller's compiler emits
`ld r2, 40(r1)` immediately after every `bl __<name>` to restore TOC
across a cross-module call.  The reference SCE compiler does this; our
PPU GCC + PSL1GHT linker treats the stub archive as intra-DSO (because
it's statically linked into the same binary) and leaves the trailing
`nop` un-rewritten.  Without the caller-side restore, `r2` holds the
SPRX TOC after return and the next TOC-relative load reads garbage.

**Why `stdu`-frame wrapping doesn't work either.**
A wrapping trampoline (`stdu r1,-N(r1); ...; bctrl; addi r1,r1,N; ...; blr`)
fixes `r2` (it's restored before `blr`) but shifts SP across the
`bctrl`.  The SPRX prologue does its own `stdu r1,-frame(r1)` and
accesses caller-passed stack args at `NEW_r1 + frame + 112`, expecting
that to equal the original `caller_sp + 112`.  Our extra frame shifts
that by `N` so the SPRX reads garbage from the wrong offsets.

**Working shape — frame-less wrapping.**
```asm
mflr   0
std    0, 24(1)        ; save caller LR in caller-frame scratch slot
std    2, 40(1)        ; save caller TOC in standard slot
lis    12, slot@ha
lwz    12, slot@l(12)
lwz    0, 0(12)        ; SPRX entry EA
lwz    2, 4(12)        ; SPRX TOC
mtctr  0
bctrl                  ; call SPRX (LR clobbered, comes back here)
ld     2, 40(1)        ; restore caller TOC
ld     0, 24(1)        ; restore caller LR
mtlr   0
blr
```

Key invariants — break any of them and one class of caller starts
crashing again:

- **No `stdu`.**  SP unchanged across `bctrl` so the SPRX finds caller-
  passed stack args at `caller_sp+112+`.
- **`bctrl`, not `bctr`.**  We need to re-enter the trampoline post-call
  to restore `r2` and LR ourselves; we can't depend on caller TOC restore.
- **LR save at `sp+24`, NOT `sp+16`.**  The SPRX's prologue writes its
  own LR to caller's `sp+16` (per ELFv1) and would clobber ours; `sp+24`
  is reserved scratch the SPRX never touches.
- **`r2` save at `sp+40`.**  Standard ELFv1 TOC save area, untouched by
  SPRX.

**How to validate after touching the stub generator.** Rebuild a sample
that exercises a >8-arg SPRX function and run it on RPCS3.
`samples/spurs/hello-ppu-spurs-job-chain` is the canonical regression
test — its `cellSpursJobChainAttributeInitialize` call has 14 args and
won't return success unless both invariants above hold.

**Future work.** If we ever bring up a Canadian-cross host toolchain
(MinGW-hosted PPU compiler) or migrate to ELFv2, this assumption needs
revisiting.  The "PPU GCC linker rewrites the trailing `nop` after
cross-module `bl`" path was load-bearing for the reference stub shape
and is currently false in our setup; if we ever fix it we can switch to
the smaller `bctr` tail-call form.

---

## Struct pointer fields crossing Cell SPRX boundary need ATTRIBUTE_PRXPTR

**Status:** operational note. Applies to every new `cell/*.h` struct
whose memory layout is consumed by a runtime-loaded Cell SPRX
(cellJpgDec, cellAudio, cellPngDec, etc).

The reference SDK compiles with a 32-bit-pointer ABI globally; every
Cell SPRX expects "4-byte pointer at offset X" from the caller's
struct. Our PPU64 toolchain uses natural 8-byte pointers, so a plain
`void *field;` occupies 8 bytes and shifts every downstream field by
4 (plus alignment padding). The SPRX reads offsets that no longer
match and dereferences junk.

**Fix:** every pointer field (including function pointers) in a
struct that the SPRX reads must be tagged `ATTRIBUTE_PRXPTR` (expands
to `__attribute__((mode(SI)))`). Include `<ppu-types.h>` for the
macro. Pure-output structs the caller only reads (CellXxxInfo,
CellXxxOutParam returned from ReadHeader / SetParameter) don't need
it unless they chain into a later SPRX call.

**Symptom:** crash inside the SPRX reading an address shaped like
`0x00000000XX000000` or `0x0000000XXX00000000` (32-bit value landing
in the high half of a 64-bit read). Runs fine up to the first SPRX
call that consumes the malformed struct. Discovered on jpg_dec port
(2026-04-24), applied to `sdk/include/cell/codec/jpgdec.h` for
`CellJpgDecThreadInParam` / `CellJpgDecSrc` / `CellJpgDecInParam`.

Handle types warrant the related pattern: when the reference header
declares a handle as `void *`, prefer `typedef uint32_t CellXxxHandle;`
in ours. The SPRX writes 4 bytes into `*handlePtr` at Create; a
`uint64_t` or `void *` destination leaves the low 4 bytes undefined
and handle-roundtrip fails with `CELL_XXX_ERROR_ARG`. Pattern already
in pngdec and jpgdec handles.

---

## Cell-SDK-compat struct-field aliases live in SDK, not in PSL1GHT

**Status:** fixed. Documented here for the history of how it
evolved, because the patches/psl1ght/ tree still carries two
retired patches (0025, 0026) that describe the original PSL1GHT
edits before the fix moved over.

PSL1GHT's struct fields use camelCase (`antiAlias`,
`resolution`); the cell-SDK sample code writes them lower (`antialias`,
`resolutionId`). The PSL1GHT patches added anonymous unions so
both names resolve to the same u8; we've since moved that aliasing
into our own SDK structs (`CellGcmSurface`, `CellVideoOut*`) so
PSL1GHT's tree stays unmodified.

---

## cellGcmSetTimeStamp was emitting invalid FIFO words via PSL1GHT

**Status:** fixed.

PSL1GHT's `rsxSetTimeStamp` (in `librsx/commands_impl.h`) reserves
2 FIFO slots but writes to slot `[2]` instead of `[1]`, leaking
past the reservation and leaving the arg word uninitialized. On
RPCS3 that surfaces as `NV4097_GET_REPORT: Bad type 0` spam every
frame and, over thousands of frames, accumulates into enough FIFO
corruption that the `spinning-cube` sample visibly stops rendering
without crashing. Replaced by a native emitter in
`sdk/include/cell/gcm/gcm_command_c.h::cellGcmSetTimeStamp` that
uses our own `ps3tc_gcm_reserve` helper — arg encoding matches the
canonical `offset | (type << 24)` from the reference cell SDK's
`gcm_methods.h`.

---

## FIFO-wrap callback ABI had a latent register-clobber bug

**Status:** fixed.

The inline asm trampoline used to invoke the firmware-registered
FIFO-wrap callback (`ps3tc_gcm_invoke_callback` in
`sdk/include/cell/gcm/gcm_cg_bridge.h`, mirrored in PSL1GHT's
`rsx_function_macros.h`) relied on GCC coincidentally keeping the
`ctx` and `count` function arguments in r3/r4 across the asm
block. The asm never explicitly moved them into the ABI-required
registers and never clobbered r3/r4/caller-saved regs. Adding any
code before the asm (e.g. a printf, a counter increment) shuffles
GCC's register allocation enough to call the firmware callback
with garbage in r3 — at which point the firmware dereferences
`ctx->current` through garbage and segfaults.

Our version now does explicit `mr 3, %1` / `mr 4, %2` inside the
asm and lists r3/r4/r5…r12/cr0/cr1/lr/ctr/memory in the clobber
list, so it's robust to any arrangement of code before the asm.
PSL1GHT's mirror still has the original form — it only works
because the function has no body before the asm. Any user who
copies the pattern and adds code breaks it.

---

## Firmware FIFO-wrap callback deadlocks on RPCS3 after ~140 frames

**Status:** fixed.

The default wrap callback `cellGcmInit` installs via
`gcmInitBodyEx` writes the JUMP command back to `begin` but never
advances the PUT register, so the GPU never sees the JUMP, and
the callback spins waiting for GET to wrap forever. The sample
shows up as "cube stops rendering at ~frame 145" with no error
in the RPCS3 log. Documented in
`sdk/libgcm_cmd/src/ps3tc_fifo_wrap.c`. Our native callback
(`ps3tc_fifo_wrap_install`) takes over `ctx->callback` right
after `cellGcmInit` to dodge this entirely.

---

## PSGL bindings — not shipped (maybe later)

**Status:** open question; intentionally deferred.

The original PS3 runtime offered two graphics paths: low-level GCM
(direct command-buffer construction, what our SDK targets) and PSGL
(an OpenGL-ES-1.1-flavoured wrapper sitting on top of GCM, with its
own header tree, runtime library, and shader-build pipeline via
`psgl_shader_builder`).  Our SDK ships the GCM surface only.  Code
written for older SDKs that imports `<PSGL/psgl.h>`, calls
`psglInit` / `psglGetDeviceDimensions`, or expects `glActiveTexture`
/ `glClientActiveTexture` / `GLuint` against an OpenGL-ES symbol set
won't link.

Symptoms when porting such code:

```
error: 'glActiveTexture' was not declared in this scope
error: 'psglGetDeviceDimensions' was not declared in this scope
error: 'GLuint' was not declared in this scope
```

Reproduces today on framework code that includes both a GCM-shape
window class (`FWCellGCMWindow`) and a PSGL-shape one
(`FWCellGLWindow`); the framework's own Makefile builds both
unconditionally.  Workaround for a sample build is to drop the GL
window source from the framework's source list — the GCM window
covers everything most samples actually use at runtime.

**Why it's a maybe rather than a no.**  Some older code paths
(particularly UI overlays, font rendering helpers, and a handful of
graphics-tutorial samples) lean on PSGL's higher-level API surface.
A future PSGL implementation could either:

1. Author a thin PSGL-on-GCM shim that maps the PSGL entry points
   onto our existing GCM surface (the original PSGL was implemented
   roughly this way; the OpenGL-ES-style state machine is a thin
   layer over the underlying RSX command stream).  Practical scope:
   roughly the same order of magnitude as our current `libgcm_cmd`
   plus the `cellGcmCg*` helpers.
2. Skip PSGL entirely and migrate any code that needs it to direct
   GCM, treating the PSGL absence as a permanent deprecation.

We haven't decided.  Today's stance: build samples that need PSGL
fail at link with a clear "no PSGL" indicator; if a real port shows
up needing the bindings, we'll re-evaluate based on its scope.

**For homebrew/sample porting today:** if the sample only uses
PSGL for the window-and-input scaffolding (the common case for the
graphics tutorials), drop the GL framework files and switch to the
GCM window class — the rendering inside still uses GCM regardless.
