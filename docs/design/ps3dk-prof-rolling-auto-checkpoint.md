# Private AUTO rolling restart checkpoint

`auto_rsx::rolling` preserves bounded exact support for automatic RSX
observation, types 34–38. It returns actual retained operation intervals and frame
views with explicit boundary qualifiers. It does not enable runtime rolling,
provide a combined-file position proof, authenticate a prefix, or implement the
manual GPU event family's PROFILE-gated RSX MEASURED analysis.

The adapter reuses the ordinary AUTO Reader. No fabricated operation, FRAME mark,
CONFIG, STATUS or PROFILE is supplied to that reader. Original CONFIG, coverage
and clock support is validated at its original position; operation history is
represented by typed census and build populations. Prefix operations never become
displayed measurements, and dependency coverage rows never enter the displayed
retained frame list.

## Context and admitted input

`checkpoint`, `advance`, `read` and `analyze` require explicit limits and one
caller-owned `WorkBudget`. Context includes the original file header, accepted
original HEADER/CLOCKS/PROFILE payloads and owner/domain declarations at the cut.
The caller owns framing/CRC, chronological declarations, and common source
ancestry when pairing the result with `fifo::rolling::Retained`. Batches must be
split at declaration transitions; a final declaration dictionary is not context
for an earlier input. Retained FIFO input must have matching original cut/frontier
and compatible owner/domain/frequency. Numeric agreement is not authentication.

Standalone context masks are exactly 129/131/193/195/641/643/705/707: AUTO plus
domain extensions, optional FIFO, PROFILE and WINDOWS. These bits are preserved,
never masked away. The mask admits context for this family, not arbitrary chunks
of every advertised family. A joined reader needs a separate complete admission
and position proof. EXPORT contexts are refused.

Attached CONFIG pairs are 7/15, 15/11, 31/11, 63/11, 127/11, 255/11, 511/11 and 1023/11;
both existing REFUSED widths are supported. CONFIG 2047 remains explicitly
unsupported. Its number is unrelated to the already accepted STATUS flag mask.
The current ordinary CONFIG checks are reused, including frequency, owner and
domain declarations. No new epoch-declaration or RANGE-descriptor source rule is
invented by this adapter.

## State and encoded schema

Private schema 1 uses little-endian fields, magic `PFRAUTO1`, a 320-byte fixed
header and trailing IEEE CRC32. Unknown flags, nonzero reserved fields,
noncanonical ordering, extra bytes and inconsistent census refuse. Import also
requires byte-identical canonical re-encoding; repaired CRC is insufficient.

| Offset | Field |
|---|---|
| 0 | Magic8 |
| 8,12 | u32 version1, fixed size320 |
| 16,24,32 | u64 total bytes, exclusive original next chunk, historical work census |
| 40,44 | u32 state flags, original file flags |
| 48,56 | u64 observed frequency, original CONFIG index |
| 64,72 | u64 last operation ID, maximum publication snapshot |
| 80,84 | u32 last coverage owner, last clock-anchor ID |
| 88,96 | u64 last coverage frame, cumulative coverage lost |
| 104,112,120,128,136 | u64 OPS, OPS chunks, COVERAGE, CLOCK and STATUS counts |
| 144,152,160 | u64 framed AUTO bytes, AUTO chunks, latest STATUS index |
| 168,176,184,192,200 | u64 OPS/frames/clocks/last-ID/max-publication at saved STATUS |
| 208 | u64 last coverage index |
| 216,220 | u32 CONFIG and STATUS payload lengths |
| 224,228,232,236 | u32 final-membership runs, execution-boundary runs, build entries, anchors |
| 240,244 | u32 bootstrap count and bootstrap section bytes |
| 248–319 | Reserved zero |

Flags are CONFIG1, STATUS2, LAST_COVERAGE4, FINAL8, INVALID_EXCLUSION16,
PREFIX_UNASSOCIATED32 and CLOCK_BOUND64. Absent fields are zero; presence flags
distinguish frame0 from absence.

Sections follow in this order: original 16-byte file header; bootstrap entries;
optional CONFIG and STATUS payloads; final-membership runs; execution-boundary
runs; build entries; anchors; CRC4. A bootstrap entry is original index:u64,
type:u32, payload length:u32 and original payload. Only HEADER0, CLOCKS1 and
PROFILE17 at original index2 are allowed. Imported bootstrap identity must match
the caller's accepted context, with only actual chronological growth permitted.

A 24-byte run is `(owner:u32, zero:u32, first:u64, last:u64)`. Runs are sorted,
disjoint and nonadjacent per owner. Final membership preserves exact duplicate
coverage detection even when owners interleave and frame IDs have holes. The
ordinary reader's temporary final set is cleared after each accepted row; no
historical run is expanded into individual frame IDs.

A build entry has 80 fixed bytes: owner:u32, COVERAGE presence:u32, frame:u64,
three u64 kind populations, abandoned:u64, valid whole-CALL build-excluded:u64,
coverage index:u64, coverage payload length:u32, zero:u32, zero:u64; then its
optional original 352/384/416/480-byte coverage payload. Keys are unique and
sorted. A 104-byte anchor entry is original index:u64 plus the original 96-byte
payload. Failed observations remain in support, in original order.

The exact encoded length is:

```
340 + bootstrap_bytes + config_bytes + status_bytes
    + 24 * (final_runs + boundary_runs)
    + sum(80 + build_coverage_bytes) + 104 * anchors
```

This is not an all-history operation log. It is bounded support, not constant
memory for unrestricted capture duration: sparse membership, old build references
without a wire retirement rule, and complete anchor qualification can grow.

## Validation and retained analysis

Every genuine FINAL still reconciles prefix plus suffix operation/frame/clock
counts. STATUS conservation, sticky flags, serial frontiers and the post-FINAL
fence remain ordinary Reader checks. A saved provisional STATUS can precede newer
prefix records; its saved census is validated separately from the current census.
Original AUTO byte/chunk/work equations and exact membership/cardinality checks
are preserved. Imported summaries are producer claims until checked against an
actual prior checkpoint and canonical advancement; they do not prove an unseen
history or disjoint global chunk positions.

Import also rejects scalar histories that cannot be encoded by the ordinary
source reader. With `O` parsed operations, `C` OPS chunks and negotiated row width
`w`, let `M = floor((MAX_CHUNK_LEN - 16) / w)`. Nonempty lists require
`C <= O <= C*M`. For a saved STATUS at parsed row count `Q`, atomic lists require
`ceil(Q/M) + ceil((O-Q)/M) <= C`: a STATUS cannot divide one OPS chunk.
The parsed ID increase since that snapshot must accommodate `O-Q` later rows;
this uses saved parsed IDs, not STATUS's possibly leading admitted-ID frontier.
REFUSED CONFIG permits zero statuses or its single genuine FINAL. In all cases,
the number of statuses cannot exceed `last_status_index - config_index`.
Products and sums use widened arithmetic; differences are checked before use.
Sparse IDs, batched lists, provisional STATUS and unrelated-family position holes
remain valid. These bounds do not establish a full cross-family slot assignment.

Build population includes prefix support and suffix contributions under actual
build tokens. Execution population contains actual retained rows under execution
tokens. This permits a retained CALL frame to use an evicted build row while
keeping the two populations separate. Per-kind counts, abandoned draws,
whole-CALL exclusions, historical invalid exclusions and sticky replay/quarantine
remain relevant. Typed unavailability is derived from evidence predicates, never
from human-readable reason strings.

A prefix-touched execution frame is boundary-censored. Later duplicate FRAME
marks cannot stand in for evicted first valid marks. Raw retained intervals can
remain useful while a whole-frame verdict is unknown. FIFO prefix membership or
unassociated prefix events prevent an empty retained wait list from becoming a
zero-wait conclusion. Missing FINAL, incomplete phases, replay, clock regression,
and presentation qualifiers remain explicit. FIFO43 remains partial-row evidence.

Qualification uses every original anchor with the existing valid-observation
parities, affine constraints, holdouts and nearest-anchor bounds. Old variants'
failed/lost-anchor rules and bracketed variants' regression rule remain unchanged.
No extrapolation or cached Qualified verdict substitutes for support. AUTO's map
does not establish a reviewed hardware premise or manual RSX MEASURED state.

The ordinary and rolling reports share the frame decision loop through a
crate-private support view. The ordinary path keeps its arithmetic, branch order,
strings and missing/refused CONFIG early return. Rolling uses a caller-metered
qualification helper and reserves conservative work/temporary bounds before
repeated scans, sorting, exact arithmetic and output allocation.

## Bounds and tests

Defaults: 8 MiB checkpoint, 6 MiB support, 4096 anchors/build entries/membership
runs, 262144 retained records, 64 MiB input, 64 MiB temporary storage and 2^28 work
items per call. The outer work budget persists across calls. These are separate
ceilings, and analysis reservations are conservative rather than tight. Exhaustion
returns a named Limit before publishing successor state. It is not source
corruption, fabricated LOSS, or permission to silently discard required support.

`tests/rolling_auto.rs` contains independent wire fixtures and explicit expected
populations, mapped intervals and an interior RSX-bound verdict with evicted build
support and paired FIFO. It also covers canonical cuts, final/status snapshots,
boundary exclusions, historical provenance/replay, failed clocks, repaired-CRC
checkpoint controls and all 1024 ten-bit masks.

`tests/host/ps3dk-prof-rolling/run_auto_mutants.py` runs the pinned 29-test baseline
and 26 isolated sensitivity controls, serially. It never modifies the checkout;
only each designated assertion failure counts as detection. Its check-only mode
validates pins/anchors without invoking Cargo. Actual execution requires a caller
build slot, a cached offline dependency registry and a new output directory.
The REFUSED-count control also bypasses the positional guard because that witness
violates both; it is not an independent kill of each guard. The separate attached
STATUS-position control tests the positional obligation. The snapshot-bound
control bypasses its redundant checked subtraction as well as its original bounds.

This unit's tests do not enable physical rolling capture or prove arbitrary
history equivalence. Combined-family position proof, full reader/report regression,
resource-stop integration and any target evidence remain separate gates.
