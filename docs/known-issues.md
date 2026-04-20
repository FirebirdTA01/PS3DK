# Known issues, limitations, and planned improvements

Running list of things the SDK currently works around, defers, or does
in a less-than-ideal way — each entry pairs the observed symptom with
the planned fix so we don't lose track of them when moving on to the
next surface.

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
   a short smoke test is not sufficient.

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

## <cell/dbgfont.h> tier-1 stub redirects to TTY; real on-screen renderer TBD

**Status:** tier-1 done (text visible in RPCS3.log), tier-2 (on-screen
text overlay) deferred.

**Tier-1 — what's implemented today.**
`sdk/include/cell/dbgfont.h` routes `cellDbgFontPuts` / Printf to
stdout with a tagged prefix:

    [dbgfont 0.05,0.10 s=1.00 #ff00ffff] Drawing Time = 2.3450 msec

— which lands in RPCS3's TTY.log.  `cellDbgFontInitGcm` / ExitGcm
/ DrawGcm remain no-ops: Printf/Puts already flushed the text
inline, so there's nothing per-frame to draw yet.  Sample source
that writes debug overlays via Printf now produces visible output
without an on-screen renderer.

**Tier-2 — plan.**
Port PSL1GHT's `samples/graphics/debugfont_renderer/` as a proper
`libdbgfont.a`.  The inputs it needs are all already in our tree:

- Font bitmap — `samples/graphics/debugfont_renderer/source/
  debugfontdata.h` (1373-line static array, 8-bit alpha, 128×128
  atlas, printable ASCII).
- Vertex-program source — `.../shaders/vpshader_dbgfont.vcg`
  (passthrough: pos/color/texcoord → NDC with identity transform).
- Fragment-program source — `.../shaders/fpshader_dbgfont.fcg`
  (tex2D .w → if > 0.5 emit vertex color, else discard alpha).

Steps:

1. New `sdk/libdbgfont/` directory with the font data + shaders
   copied in, plus a Makefile that compiles the shaders through
   sce-cgc (same path the reference samples use) or our own
   `rsx-cg-compiler` and embeds them as C byte arrays.
2. Rewrite PSL1GHT's C++ renderer as C, swapping the rsx* calls
   for the corresponding cellGcm* names that already exist in our
   SDK (`cellGcmSetTexture`, `cellGcmSetVertexDataArray`,
   `cellGcmSetDrawArrays`, etc.).
3. Move the `<cell/dbgfont.h>` function bodies out of the header
   and into the library; header becomes pure declarations.
4. Update `sdk/Makefile` to treat `libdbgfont` as a sublib
   alongside `libgcm_cmd`.
5. Samples that want real overlay text link `-ldbgfont` in addition
   to `-lgcm_cmd -lrsx ...`.

Accept/test criteria: port a reference sample that uses Printf (cube,
alphakill) with the tier-2 library and confirm the overlay text
draws on top of the frame in RPCS3.

---

## hello-ppu-cellgcm-triangle sample: slow + occasional crash during PS-menu interaction

**Status:** sample-local bug.  Reference samples (`basic`, `cube`,
`flip_immediate`) that go through the same SDK code paths run cleanly;
only our in-tree `samples/hello-ppu-cellgcm-triangle` exhibits this.

**Symptoms.** The triangle renders correctly, but the PS home menu is
sluggish to appear, inputs respond slowly while the sample is running,
and navigating back and forth in the menu a few times occasionally
crashes the emulator.  Reference samples in the same session with the
same SDK install don't reproduce any of it.

**Likely causes to check first when revisiting:**

1. Our `ioPadGetInfo` / `ioPadGetData` calls in the main loop may be
   hitting the pad subsystem without an explicit `ioPadInit` setup,
   so PS-menu overlay input contends with our polling.
2. The buffer-state labels (`LABEL_BUFFER_STATUS_OFFSET + idx`) aren't
   pre-seeded to `BUFFER_IDLE` before the main loop.  The first
   `cellGcmSetWaitLabel(..., BUFFER_IDLE)` inside `flip()` may stall
   waiting for a value that only arrives after a flip that can't
   happen yet.
3. `GCM_FLIP_HSYNC` fires flip events more often than vblank; our
   vblank handler's `on_flip` guard flag may race with the rapid
   re-entry when the PS-menu overlay is active.

**Why we're not fixing it now.** Doesn't block any reference-sample work or
SDK progress — the triangle is our own test harness, not a user
deliverable.  Revisit when the next SDK surface we touch is flip /
pad related so the triage sits naturally in that session.

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
