# Known issues, limitations, and planned improvements

Running list of things the SDK currently works around, defers, or does
in a less-than-ideal way — each entry pairs the observed symptom with
the planned fix so we don't lose track of them when moving on to the
next phase.

---

## FIFO wrap drain-wait causes occasional frame flicker

**Status:** works correctly (no hangs, no desync), but drops the
occasional frame on a wrap.

**Symptom.** The `sony-cube` sample runs indefinitely with our native
wrap callback, but exhibits an occasional mild flicker — one dropped
frame each time the command FIFO wraps. In practice this lands
roughly every ~1.25 s at 60 fps with the sample's default
`CB_SIZE = 0x10000` (64 KB) and ~800 FIFO bytes per frame.

**Where it is.** `sdk/libgcm_cmd/src/ps3tc_fifo_wrap.c` —
`ps3tc_fifo_wrap_callback`. It's a single-buffer in-place wrap:
writes the JUMP-to-begin command, sets `ctrl->put = begin_off`, then
spins on `ctrl->get != begin_off` via `sys_timer_usleep(30)` until
the GPU follows the JUMP. That spin **is the drain** — PPU is
blocked until the GPU has processed every command ahead of it. If
the wait lands mid-frame, the frame's budget blows past VSYNC and
you see a visible stutter.

**Why the simple double-buffered version didn't help.** An attempt
at splitting the FIFO into two halves made it *worse*, not better.
Halving the buffer doubled the wrap frequency (~2-3x more frequent),
and each half-wrap still does the full drain-wait, which meant the
drain-stall was happening two to three times as often. Combined
with the sample's `cellGcmFinish(1)` using a constant reference
value (effectively a no-op after frame 0, so PPU races ahead), the
PPU/GPU timing went bursty and the cube visibly jumped between
rotation states on every wrap cycle. Reverted.

**The correct fix — back-end-label / semaphore async sync.** The
drain-wait has to go. The canonical way libgcm does this on real
PS3 is:

1. At a safe reuse point inside each half of the FIFO (e.g. the
   midpoint or a few commands before the JUMP), emit
   `cellGcmSetWriteBackEndLabel(label_idx, sentinel_value)`. When
   the GPU processes that command it writes `sentinel_value` into a
   memory slot PPU can read via `cellGcmGetLabelAddress(label_idx)`.
2. Our wrap callback maintains a separate sentinel per half.
   Before reusing half A, it polls the half-A label address; if it
   already holds the expected sentinel, GPU has finished with half
   A and PPU can start writing it immediately — **no wait**. Only
   if the GPU is still catching up does the callback spin, and
   then it spins on the label address (fast memory read) rather
   than the GPU control register.
3. This is what Sony's sample code that uses `PrepareFlip` already
   does implicitly via buffer-state labels. Our wrap just has to
   do the equivalent for its own command-buffer halves.

In steady state (GPU within a frame of PPU, which is what VSYNC +
`waitFlip` enforce), the label is already written by the time PPU
wraps back, and the callback collapses to a single memory load +
comparison. No drain, no frame drops.

**Why we're not doing it right now.** It's a meaningful chunk of
work: lazy label-slot allocation, emission of the label-write
commands at the right points in the stream, one-per-half
bookkeeping. It's worth waiting until we have more Sony samples
running so we can be sure the design generalizes (e.g. some games
use more than two FIFO halves, or very large FIFOs where the
back-label pattern needs different spacing). The current
single-buffer-with-drain-wait version is correct under all
observed workloads; it just flickers. We revisit when the next
sample shows a case where the flicker becomes a blocker rather
than a mild annoyance.

---

## hello-ppu-cellgcm-triangle sample: slow + occasional crash during PS-menu interaction

**Status:** sample-local bug.  Sony samples (`basic`, `cube`,
`flip_immediate`) that go through the same SDK code paths run cleanly;
only our in-tree `samples/hello-ppu-cellgcm-triangle` exhibits this.

**Symptoms.** The triangle renders correctly, but the PS home menu is
sluggish to appear, inputs respond slowly while the sample is running,
and navigating back and forth in the menu a few times occasionally
crashes the emulator.  Sony samples in the same session with the
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

**Why we're not fixing it now.** Doesn't block any Sony-sample work or
SDK progress — the triangle is our own test harness, not a user
deliverable.  Revisit when the next SDK surface we touch is flip /
pad related so the triage sits naturally in that session.

---

## Sony-compat struct-field aliases live in SDK, not in PSL1GHT

**Status:** fixed. Documented here for the history of how it
evolved, because the patches/psl1ght/ tree still carries two
retired patches (0025, 0026) that describe the original PSL1GHT
edits before the fix moved over.

PSL1GHT's struct fields use camelCase (`antiAlias`,
`resolution`); Sony's sample code writes them lower (`antialias`,
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
corruption that the `sony-cube` sample visibly stops rendering
without crashing. Replaced by a native emitter in
`sdk/include/cell/gcm/gcm_command_c.h::cellGcmSetTimeStamp` that
uses our own `ps3tc_gcm_reserve` helper — arg encoding matches
Sony's canonical
`offset | (type << 24)` from `reference/sony-sdk/target/ppu/
include/cell/gcm/gcm_methods.h`.

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
