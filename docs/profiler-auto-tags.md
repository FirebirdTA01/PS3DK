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

Ranges come from a reserve of report pairs: by default an eighth of the pool, at most 64 (63 with the default 1024
reports), or the `range_pairs` field of `ps3dk_prof_auto_options` (refused when it would leave fewer than 64 pairs for
draws and frame marks). `auto` prints the reserves. Nesting is allowed up to depth 4. A begin is refused, and the
refusal is counted in `ranges_refused` in `auto --json`, when it would nest deeper than 4, when no pair is free, when it
is begun inside a segment build, or when the range is ended in another frame than the one it was opened in.

A pair returns to the reserve only after the RSX has passed it and its record has been read. Size the reserve for the
ranges outstanding across that lag, not for one frame: with the RSX about a frame behind and the drainer up to another
frame behind it, about three frames of ranges are in flight, so the default 63 holds 19 ranges per frame plus margin.
Check that `ranges_refused` stays 0. The reserve does not depend on `PS3DK_PROF_AUTO_TIMED_RANGES_ONLY()`: timed ranges
still time their draws, from the draw pairs.

### Return values

The range macros are `int` expressions. `PS3DK_PROF_AUTO_RANGE_BEGIN` and `_BEGIN_TIMED` yield 1 when the range was
admitted and 0 when it was refused; `PS3DK_PROF_AUTO_RANGE_END()` yields 1 when the range was committed (its record
will be written) and 0 when it was refused. Without `PS3_PROF_AUTO` all three are a constant 1 and call nothing:
nothing is refused there, so the same BEGIN/END pairing and counting logic behaves the same in both builds. A refused begin still occupies its nesting level, so always issue the matching end, whatever the begin
returned:

```c
int admitted = PS3DK_PROF_AUTO_RANGE_BEGIN("lighting");
/* ...lighting draws */
int committed = PS3DK_PROF_AUTO_RANGE_END();   /* always, even when admitted == 0 */
if (!admitted || !committed) my_range_misses++;
```

The name must be a string literal (the runtime keeps one descriptor per name, file and line, up to 256 call sites; a
begin at a further site is refused, still balanced).

### Windows opened mid-frame

A frame normally gets its coverage row at its boundary. An immediate window (`PS3DK_PROF_SECTION`,
`PS3DK_PROF_CAPTURE_WHOLE_RUN`) opened in the middle of a frame, by the owner or any other thread, enters a frame that
had no row: the owner's first draw or range inside the window opens that frame's row from that moment. The row has no
FRAME marks and is always OPEN, so its ranges and draws are recorded but the frame gets no whole-frame verdict (nothing
observed before the window opened is attributed to it). Before the owner's first frame there is no frame at all:
ranges there are refused and counted only in the run total.

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

## Options before cellGcmInit

`ps3dk_prof_auto_attach` overrides the defaults only before any command is written after `cellGcmInit`
(`PRIOR_WRITES_UNKNOWN` otherwise). To set `range_pairs` or other options without that race, set them before init:

```c
ps3dk_prof_auto_options o = PS3DK_PROF_AUTO_OPTIONS_INIT;
o.range_pairs = 96;
PS3DK_PROF_AUTO_OPTIONS_SET(&o);     /* before cellGcmInit, on the thread that calls it */
cellGcmInit(command_size, io_size, io_address);
```

The options are validated and copied at the call (EINVAL for options the attach would refuse; the earlier 32-byte
struct is accepted and takes the default range reserve) and used by the intercepted `cellGcmInit` in place of the
defaults. Only an init on the same thread uses them; the first successful init consumes them, and a later call is EBUSY.
When the init runs on another thread the defaults are used and the capture says so: `auto` prints "pre-init options
ignored: set on a different thread than cellGcmInit" (STATUS flag 2048, `preinit_options_ignored` in `auto --json`).
Without `PS3_PROF_AUTO` the macro is 0 and calls nothing.

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
- A frame entered mid-frame by a window gets an OPEN row from the owner's first draw or range: no whole-frame verdict.
- A range begun inside a segment build, or ended in a different frame, is refused and counted.
- Untimed draws add nothing to any tag's merged busy time; their cost shows only in the frame's untimed counts. A
  segment's untimed draws are still covered as a block when its CALL is bracketed.
- `fp_offset` is not stable across boots; correlate it through the tag and your own bind order.
