# Next-session prompt — Phase 8 stage 4: loop support + reference-sample gap hunt

Paste this as the opening user message of a fresh Claude Code session in the
PS3 Custom Toolchain repo. It is self-contained — no prior context required
beyond what's checked into the repo and `~/.claude/projects/.../memory/`.

---

We're closing the last open item in Phase 8 stage 4 of the rsx-cg-compiler
(our own Cg→NV40 compiler at `tools/rsx-cg-compiler/`): **for-loop support**.
At the same time, we want a reference SDK sample driving the work as a real-
world validation test, in parallel with the compiler implementation.

## Status check before doing anything

The Phase 8 stage 4 memory entry was stale until the previous session updated
it. Current truth (verified 2026-04-24, commit `c47a4bd`):

- `tests/run_diff.sh` reports **46/46 shaders byte-match the reference Cg
  compiler** at `--O2 --fastmath`.
- length / normalize / rcp / rsqrt — DONE (commit `f754ff3`).
- Simple if-only and if-else diamonds — DONE (commit `38e163a` via if-convert
  to Select, no real branch opcodes emitted).
- Loops — STILL PENDING. No `for_*.cg` / `loop_*.cg` test shaders exist.
- Multi-instruction if-else diamonds (e.g. `if { a*b + c; }`) — PENDING.
  Only single-stout branches match the if-convert shape today.
- If-only with a varying default — PENDING (would need MOVH-promote of the
  varying once for both preload and compare).

Run `bash tools/rsx-cg-compiler/tests/run_diff.sh` first thing to confirm
46/46 still passes and the harness works — that's the gate on every change.

## The blocker (don't skip the diagnostic)

Per the existing project memory `project_phase8_stage4_status.md` "Known
gaps (deferred)" section: loops are blocked in the **donor IR builder**, not
in the NV40 emitter. Specifically:

> Loop-carried SSA values aren't threaded through Phi nodes. e.g.
> `for (int i=0; i<4; ++i) acc *= a` emits `mul %vcol, %a` inside the
> loop body rather than `mul %acc_phi, %a`. The reference compiler
> unrolls static-count loops and emits real LOOPH/SLTRC/BRKH/ENDLOOP
> for variable ones; we can't match either until the IR carries the
> acc value correctly. Fixing this requires an upstream-style change
> in `ir_builder.cpp` (emit Phi at loop-header blocks + rewrite
> loop-body value lookups).

**Confirm this is still the blocker.** Write a tiny loop test shader (see
below), drop it in `tools/rsx-cg-compiler/tests/shaders/`, run `run_diff.sh`,
and look at what our compiler emits vs the reference compiler's output. If
the IR really is producing `mul %vcol, %a` instead of carrying `acc` through
a Phi, that's the entry point. If it has changed since the diagnosis was
written, adapt.

## Reference sample driving the work

`reference/sony-sdk/samples/sdk/graphics/gcm/stereo_font/shader/fpshader_filter.cg`
(29 lines — the smallest reference shader in the SDK with a `for` loop). It's
a 3-tap convolution with a static-count `for (int i = 0; i < 3; i++)` loop
that accumulates into a `half3 result`. The reference compiler unrolls this;
matching it means our IR has to carry the accumulator through unrolled body
copies.

Note: there are **zero `while` loops** in the entire reference SDK, so don't
spend time on `while` — `for` (especially static-count for) is the only
form that needs to work for any real shader.

## Strategy — two tracks in parallel

### Track A: compiler — loop support with byte-diff gate

1. **Add the smallest possible loop test shader** to
   `tools/rsx-cg-compiler/tests/shaders/`. Start so small it can't possibly
   fail for ancillary reasons. e.g. `loop_static3_f.cg`:

   ```cg
   void main(
       float4 vcol      : COLOR,
       out float4 color : COLOR)
   {
       float acc = 0.0f;
       for (int i = 0; i < 3; i++) {
           acc += vcol.x;
       }
       color = float4(acc, 0, 0, 1);
   }
   ```

   Verify the reference compiler produces a sensible unrolled output. If it
   declines to compile (unlikely with a static count) or produces something
   surprising, capture the actual ucode — that's the target.

2. **Run `run_diff.sh`** and read the diff. Look at:
   - Whether the IR builder emits 3 unrolled body copies or one body with a
     loop edge. (The reference compiler unrolls statics; we need the same.)
   - Whether the unrolled bodies thread `acc` through Phi-equivalent values
     or each one re-reads `vcol.x` (the broken case).

3. **Fix the IR builder.** The donor IR builder lives somewhere under
   `tools/rsx-cg-compiler/src/donor/`. Before editing, list the files
   (`ls tools/rsx-cg-compiler/src/donor/` then look for `ir_builder.cpp`
   or similar) and grep for `for` / `while` / `loop` / `Phi` to orient.
   The fix shape is: at a loop-header equivalent, emit a Phi node that
   names the loop-carried variable; rewrite reads of that variable inside
   the body to read the Phi result. Static-count loops can be handled by
   unrolling the body N times and threading the Phi value forward.

4. **Iterate to byte-match** on the static-count case first. Then add a
   second test shader matching the stereo_font pattern more closely (a
   3-tap accumulator with `result.r += dot(...)`) and get that byte-exact.
   Don't move to variable-count loops (real LOOPH/BRKH opcodes) until the
   static-count Phi plumbing is solid — those are a separate workstream.

5. Suite gate: `46/46` → `47/47` → `48/48` byte-match before committing.

### Track B: sample — port stereo_font as a real build target

stereo_font is large (multi-shader sample with font, menu, character, post-
effect, SPU code). Even before our compiler can emit its loop shader, port
the **build infrastructure** so the SDK-coverage gaps surface:

1. Build it under `/tmp/stereo_font/` (per `feedback_sony_sample_build_location`
   memory — never under repo tree, even build/). VPATH into
   `reference/sony-sdk/samples/sdk/graphics/gcm/stereo_font/` so we don't
   copy the source.

2. Use the reference SDK's bundled Cg compiler binary for the shader compile
   in the meantime (PSL1GHT's `cgcomp` is a viable alternative if the
   bundled binary isn't wired into the sample's Makefile here). Check
   `samples/gcm/hello-ppu-cellgcm-triangle/Makefile` for the
   `USE_RSX_CG_COMPILER=1` toggle pattern, which lets a sample swap between
   the reference binary and our compiler.

3. Expect SDK gaps. stereo_font likely uses `cellFont`, `cellFontFT` or
   similar font APIs that are nowhere in our `sdk/include/cell/`. List them
   as they surface (`grep -rE 'cellFont|cellResc|...' the sample`), check
   if any of our `nidgen` YAMLs already cover them, and if not, port the
   header + add the YAML. Pattern: see `e1c576a sdk: port cellAudioOut surface`
   from this session for a complete worked example of this surface-add
   pattern.

4. Don't try to actually run stereo_font in RPCS3 until both tracks
   converge — it's a milestone, not a check-in gate.

## House rules (already in CLAUDE.md, but worth re-stating)

- Byte-exact gate: `tests/run_diff.sh` MUST report N/N before any compiler
  change is committed. "Looks correct" doesn't count
  (`feedback_byte_exact_shader_output`).
- No vendor name in tracked content — say "reference SDK", not the
  vendor's name (`feedback_no_vendor_name_tracked`). The bar extends to
  individual tool names too (e.g. don't write the reference Cg compiler's
  exact binary name in tracked text — say "the reference Cg compiler"
  instead). RE notes go to memory or gitignored paths, never tracked docs
  (`feedback_re_docs_out_of_repo`).
- After each major rebuild, re-verify all 34 samples build green. Build all
  with `for mk in $(find samples -name Makefile -not -path "*/build/*" -not
  -path "*/spu/*"); do make -C $(dirname $mk) clean && make -C $(dirname $mk);
  done`.
- Reference SDK is at `reference/sony-sdk/` (symlinked from
  `~/cell-sdk/475.001/cell/`). It's a coverage oracle, never copied into
  tracked content.

## What to report at the end

Whether 47/47 (or higher) byte-match held, what the IR-builder fix shape
ended up being, and a list of any SDK surface gaps stereo_font surfaced
(even if not yet ported).

If you hit a new blocker — e.g. the reference compiler emits real LOOPH for
the static-count case (would be surprising but possible at certain --O
levels), or the IR builder turns out to be more entangled than the existing
diagnosis suggests — pause and report rather than burn an hour on the wrong
fix.
