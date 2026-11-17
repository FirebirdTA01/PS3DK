# Profiler auto-attribution API

## Overview

On top of the automatic RSX observation, four public calls let an application name its own work and time it
individually:

- `PS3DK_PROF_AUTO_TAG(u32 t)`: set the tag every later AUTO record carries.
- `PS3DK_PROF_AUTO_RANGE_BEGIN(name)` / `PS3DK_PROF_AUTO_RANGE_END()`: the exact RSX time of a named range, measured
  from its own report pair.
- `PS3DK_PROF_AUTO_TIMED_RANGES_ONLY()` plus `PS3DK_PROF_AUTO_RANGE_BEGIN_TIMED(name)`: spend per-draw report pairs only
  on draws inside an open timed range.

Every AUTO record also carries `fp_offset`, the last fragment-program offset set on the context through
`cellGcmSetFragmentProgram`. Without `PS3_PROF_AUTO` the calls are inert and no AUTO capture is written.

## Tagging passes and programs

`PS3DK_PROF_AUTO_TAG(t)` is called on the owner (GCM) thread and sets the tag that every later AUTO record on that
context carries. When a record takes the tag depends on where it runs: a ring draw takes it at issue, a segment draw
takes it at BUILD time, and a whole-CALL record takes it at the CALL.

`fp_offset` identifies the program a draw used: it is the offset last set through `cellGcmSetFragmentProgram` on that
context. Raw program-address writes and `rsxLoadFragmentProgramLocation` are not seen. The offset can differ between
boots of the same binary, so treat it as a per-run key and pair it with the tag.

```c
/* Pass entry: tag this pass, then bind the program it draws with. */
PS3DK_PROF_AUTO_TAG(0x1001);
cellGcmSetFragmentProgram(ctx, lighting_fp, lighting_fp_offset);
/* ...draws issued here are recorded as (tag 0x1001, that fp_offset) */
```

## Exact pass times with ranges

`PS3DK_PROF_AUTO_RANGE_BEGIN("name")` / `_END()` bracket a region of the command stream; the reader reports the exact
RSX time of the named range from its own report pair, independent of the per-draw pairs.

Ranges come from a reserve of report pairs: 16 by default, or the `range_pairs` field of `ps3dk_prof_auto_options`
at attach (refused when it would leave fewer than 64 pairs for draws and frame marks). `auto` prints the reserves. Nesting is allowed up to depth 4. A begin is refused, and the refusal is counted in `ranges_refused` in
`auto --json`, when it would nest deeper than 4, when no pair is free, when it is begun inside a segment build, or when
the range is ended in another frame than the one it was opened in.

A pair returns to the reserve only after the RSX has passed it and its record has been read. When the RSX runs about a
frame behind, two frames' ranges are in flight at once, so plan for roughly half the reserve per frame and check that
`ranges_refused` stays 0.

`ranges_refused` is a per-frame count. No capture-wide count is recorded: STATUS has none, and a range refused while
no frame is open is counted in no frame. For a capture without a FINAL status (unsealed), `auto` prints a
`tail not recorded` line and a `capture totals` line. In `auto --json` such a capture also carries `sealed: false`,
`tail` and `capture_totals`, and every frame object carries `row_state`. Each capture total is `{"value": null,
"reason": ...}` unless a STATUS was recorded. In that case `value` is the last STATUS counter and `at_least` is true.
`capture_totals.ranges_refused.retained_rows` is the sum over the frames that were kept, which is a partial count. A
frame is `OPEN` only when its row is marked open in the file. Sealed captures print exactly what they printed before.

```c
PS3DK_PROF_AUTO_RANGE_BEGIN("scene");
PS3DK_PROF_AUTO_RANGE_BEGIN("shadow");      /* nested; depth 2 of 4 */
cellGcmSetFragmentProgram(ctx, shadow_fp, shadow_fp_offset);
/* ...shadow-pass draws */
PS3DK_PROF_AUTO_RANGE_END();                /* closes "shadow" */
/* ...main-pass draws */
PS3DK_PROF_AUTO_RANGE_END();                /* closes "scene" */
```

Begin and end a range inside one frame (between two flips).

## Spending report pairs where you look

By default every draw takes a per-draw report pair. After one `PS3DK_PROF_AUTO_TIMED_RANGES_ONLY()` call, only the
draws inside an open timed range do: draws outside every timed range are counted as untimed with the reason
`outside timed range` and consume no pair. Without the call every draw is timed.

```c
PS3DK_PROF_AUTO_TIMED_RANGES_ONLY();        /* once, before the first frame */

/* per frame: */
PS3DK_PROF_AUTO_RANGE_BEGIN_TIMED("hero");
/* ...hero draws: each takes a pair and is timed individually */
PS3DK_PROF_AUTO_RANGE_END();
/* ...the rest of the frame: untimed, no pairs consumed */
```

A plain (not timed) range still reports its exact time from the range reserve while its draws stay untimed, so one
plain range per pass plus one timed range around the pass under study gives every pass's time and per-draw detail for
one of them.

## Reading the results

- `ps3dk-prof auto --ops <file>` prints each operation record with its tag and `fp_offset`.
- `ps3dk-prof auto --by-tag <file>` (or `--by-tag --json <file>`): per frame and per tag the record count, the merged
  RSX busy time and the draws a CALL ran untimed; per range name the count and the exact time; and for the whole run
  the sum and the median per frame.

Ring draws that went untimed have no record, so they carry no tag and appear only in the frame's untimed counts. A tag
can be missing for that reason even though its draws ran.

## Limits

- Range nesting is capped at 4; a deeper begin is refused and counted.
- A range begun inside a segment build, or ended in a different frame, is refused and counted.
- Untimed draws add nothing to any tag's merged busy time; their cost shows only in the frame's untimed counts. A
  segment's untimed draws are still covered as a block when its CALL is bracketed.
- `fp_offset` is not stable across boots; correlate it through the tag and your own bind order.
