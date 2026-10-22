# ps3dk-prof

Reads, checks and reports ps3dk-prof capture files, format v1. The normative format is
`docs/design/ps3dk-prof-format.md`.

```
ps3dk-prof check <file>    print the verdict
ps3dk-prof report <file>   print the verdict and the full report
```

Exit status: 0 COMPLETE, 1 INCOMPLETE, 2 MALFORMED, 3 usage or I/O error.

## Verdicts: structural pass versus whole-file verdict

- `reader::read` is the STRUCTURAL pass (sections 1-6). A successful `read` is **not** a full validity verdict:
  token rule 4 (section 9) needs the whole file, so a capture `read` accepts can still be MALFORMED.
- `load` runs `read` and then `analysis::analyze`, which applies rule 4. `check`, `report` and `compare` all go
  through `load` (`compare` takes two `Analysis` values, which only `load`/`analyze` produce).

## Values

- Durations are raw PPU timebase ticks at the capture's `freq_hz`.
- Nothing is saturated. Totals and W are exact (u128); means are rounded floating-point values, so an exact total
  does not imply an exact mean. A duration that cannot be computed, such as END before BEGIN
  (a CLOCK_ANOMALY), is shown as unavailable, never as 0. Refused and unknown values are printed as words.
- W (attributed work) is per lane, start-frame attributed and exclusive. It is not clipped to the frame interval, so
  it can exceed the frame's elapsed time, and it is never summed across lanes.

## A/B compatibility policy (`compare`)

`compare(a, b)` is `compare_with(a, b, CompareOptions::default())`. It returns a `Comparison`:
- its `kind`;
- a capture-level refusal, if any;
- the shared clock frequency;
- the recorded differences;
- the valid-lane scope of each side;
- one row per logical key (name plus logical tag).

`report::render_comparison` prints the kind FIRST, before any value.

| Kind | When | Rows |
|---|---|---|
| `Compatible` | Same workload and app, same ABI and clock, both COMPLETE | `Compared` |
| `DescriptiveCrossWorkload` | Different workload tag or app, and the caller passed `CompareOptions { allow_cross_workload: true }` | `Descriptive`: a description of two runs, NOT an A/B result, never `Compared` |
| `Refused` | Any capture-level refusal below | every row `Refused` |

| Condition | Effect |
|---|---|
| Either capture not TRANSPORT COMPLETE | The whole comparison is REFUSED. |
| Different ABI (ILP32 vs LP64) | The whole comparison is REFUSED. |
| Different or missing clock frequency | The whole comparison is REFUSED. Values are raw ticks and are not converted between clocks. |
| Different workload tag or app | REFUSED by default. With `allow_cross_workload`, the comparison is `DescriptiveCrossWorkload` and the differences are listed in `Comparison::cross_workload`. |
| Duplicate logical key in either capture | That key is REFUSED. |
| Different descriptor scope (cpu vs gpu-complete) | That key is REFUSED. |
| Censored instances, or no completed instances | That key is REFUSED. |
| Different build id, SDK version, compiler identity or flags | RECORDED in `Comparison::differences` as the intended variables of the A/B, not refused. |
| Different source file and line for a key | RECORDED in that row's `differences`, not refused. |

- **Scope:** `a_scope` and `b_scope` list the MEASUREMENT VALID incarnations each side's statistics come from.
- **Presence, per key:**
  - `OnlyInA` / `OnlyInB`: the key has spans in a valid lane of one capture and no span at all in the other.
  - `InvalidLanesOnly { a, b }`: the key has spans, but only in INVALID lanes of the flagged capture(s). It is present
    but not comparable, which is not the same as absent.

## Status

On the `feat/ps3dk-prof` branch only: not packaged in host tools, and not listed in the required-artifacts table.
