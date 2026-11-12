# Automatic RSX observation

This opt-in extension covers safe ARRAY, INDEX and CLEAR commands at their
canonical SDK emission points. It inserts two report/label markers per admitted
operation. Their interval is reached-marker elapsed time, including intervening
pipeline effects and observer work. It is not isolated draw cost, utilization,
busy percentage or proof of starvation. Unsupported forms and resource/room
refusals remain explicit coverage exclusions.

`PS3_PROF_AUTO=ON` selects the automatic lifecycle and observed GCM provider.
The intercepted GCM initialization attempts default attachment; no application
profiler call is required. An optional owner override can change the resource
range before the first operation, clock, frame or FIFO-boundary observation. CONFIG freezes at
that observation, not at an asynchronous drain. No second CONFIG or instance is
created by a later collision, reset, or failed override.

The format allocation is bit 7 (`128`) and chunks 34 through 38. Bit 0 (the
extension declarations) is required. The AUTO format does not require FIFO's bit
6; the combined provider may emit both families. Type 27 stays reserved for the
scope declaration. All fields below are little-endian, packed independently of
native C layout. Reserved bytes must be zero.

## CONFIG, type 34

Fixed 160 bytes. Schema u16 at 0 is 1; flags u16 at 2 is either 0 (attached) or
1 (REFUSED); instance u32 at 4 is 1. A successful configuration requires its
preceding owner, domain and epoch declarations.

| Offset | Type | Attached value |
|---|---|---|
| 8, 12, 16, 20 | u32 each | domain, epoch, owner incarnation, context generation |
| 24, 28 | u32 each | covered kind mask, unsupported form mask: 7/15 (first writer), 15/11 (segment and frame-mark writer) or 31/11 (presentation writer) |
| 32, 36 | u32 each | report first, report count |
| 40, 44 | u32 each | operation label, clock label |
| 48, 52, 56, 60 | u32 each | pair capacity, metadata capacity, frame capacity, record width 128 |
| 64 | u64 | PPU timebase frequency, equal to CLOCKS |
| 72, 80 | u64 each | allocated bytes, allocation ceiling |
| 88, 92 | u32 each | premise mask 3, managed exclusive lease policy 1 |
| 96 | 32 bytes | mechanism digest |
| 128 | 32 bytes | build binding |

Unsupported-form bits name unsafe=1, immediate=2, CALL=4 and direct/uninstrumented=8.
Kind bits name ARRAY=1, INDEX=2, CLEAR=4, FRAME=8 and PRESENT=16. PRESENT is not
an OPS kind: it names flip-wait scopes and polling cadence in 384-byte COVERAGE
rows (below). Exactly three pairs are valid: 7/15 for the first writer, 15/11 for
the writer that records FRAME marks and covers CALL segments built through the
segment API, and 31/11 for the presentation writer. A reader that knows only
7/15, or only 7/15 and 15/11, refuses the newer CONFIG, so it never misreads the
longer rows or reads the older poll interval as a wait.
Premise bits name report-before-label=1 and coherent stable read=2; these declare
dependencies, not proven facts. Schema 1 reserves sixteen clock reports, so the
report count is sixteen plus twice the pair capacity. The mechanism digest
identifies command/report semantics, not a measured profile. The build binding
comes from pinned pre-link inputs: every file the linker opens (found by running the
driver's final linker command, from -###, once with --verbose first) and that final command; an external
manifest binds it to the final ELF and SELF, avoiding a self-hash cycle.

A REFUSED CONFIG keeps only schema, flags, instance, frequency and both digest
fields. Offset 24 is its refusal reason u32; offset 28 is the original GCM return
code i32. Every other byte is zero. In particular, it invents no owner, domain,
epoch, lease, capacity or allocation. Unknown variants and reasons are malformed.

| Reason | Name |
|---|---|
| 1 | GCM_INIT_FAILED |
| 2 | NO_INDEPENDENT_DRAIN |
| 3 | RESOURCE_LEASE_UNVERIFIED |
| 4 | PRIOR_WRITES_UNKNOWN |
| 5 | RESOURCE_CONFLICT |
| 6 | NO_REGISTERED_OWNER |
| 7 | ALLOCATION_FAILED |
| 8 | UNSUPPORTED_CONTEXT |
| 9 | STOPPED |
| 10 | NO_GCM_INIT_OBSERVED |

**Reason 10 must be prominent in both the reader and viewer:**
`automatic observation unavailable: NO_GCM_INIT_OBSERVED`.
It means this capture never observed the selected GCM initialization path, for
example because the engine initialized GCM by another route. It is unavailable
observation, not a frame with zero RSX work. Other reasons use the same named
unavailability presentation. REFUSED CONFIG forbids OPS and CLOCK and ends with
one FINAL STATUS whose fifteen counters and gauges are all zero. It certifies
neither timing nor coverage.

## OPS, type 35

A chunk carries one or more records; the writer batches what the drainer has
completed (up to 64 per chunk) and acknowledges each record, in order, only
after the whole chunk was accepted. Readers accept any count from 1.

An operation interval runs from the RSX reaching the begin marker to reaching
the end marker. It therefore includes any work queued ahead of the operation
that the RSX was still finishing (earlier draws, a CALLed segment's preceding
commands, pipeline drains): it is not the isolated cost of that draw or clear.
The reader prints this note with every report.

Prefix 16 bytes: schema u16 at 0, reserved u16 at 2, instance u32 at 4, count u32
at 8, record width u32 at 12 (128). Count is nonzero; exact size is `16+128*count`,
within the 1 MiB chunk ceiling.

| Record offset | Type | Field |
|---|---|---|
| 0 | u64 | operation id |
| 8, 12, 14 | u32, u16, u16 | frame owner, kind, flags |
| 16, 24, 32 | u64 each | frame id, PPU before, PPU after |
| 40, 48, 56 | u64 each | raw begin, raw end, caller PC |
| 64..88 | seven u32 | original word count, start, count, offset, index type, mode, clear mask |
| 92 | u32 | segment ordinal (SEGMENT only, else reserved zero) |
| 96, 100, 104, 108, 112 | u32 each | begin report, end report, begin serial, end serial, generation |
| 116 | u32 | call ordinal (SEGMENT only, else reserved zero) |
| 120 | u64 | actual publication snapshot |

Kinds are ARRAY=1, INDEX=2, CLEAR=3 and, with kind mask bit 8, FRAME=4. Flags
are TOKEN=1, RAW_VALID=2, INCOMPLETE=4, CLOCK_INVALID=8, READ_REFUSED=16,
PUBLICATION_UNKNOWN=32 and, when CONFIG covers CALL, SEGMENT=64. Unknown bits
are refused.

A FRAME record (D2) marks one frame boundary on the RSX: role 1 (args[0]) is
emitted in the ring when the owner opens the frame, role 2 when it closes it
(explicitly, by the next frame or at owner retirement). Its report pair is
written back to back; the begin mark's raw begin and the end mark's raw end
bracket the frame's command stream on the RSX clock. It needs TOKEN and has no
other argument; it is not counted in COVERAGE. A mark refused for room or
resources is simply absent, and the frame's RSX begin or end is UNKNOWN.

A SEGMENT record (D1) is a covered operation written into a CALL segment
between cellGcmSegmentBegin and cellGcmSegmentEnd. Inside the segment it is
bracketed by two report-only commands (four words, room checked against the
segment's own end, no callback). At cellGcmSegmentCall the observer appends one
operation-label release in the ring after the CALL and its retirement label;
every record of that call shares that serial pair, so serials stay in
execution order. The record carries the segment ordinal (u32@92, nonzero) and
the call ordinal (u32@116, nonzero), and the frame of the call. A build that
is rebuilt, never called, or called outside the attached ring is committed as
INCOMPLETE with call ordinal zero and its pairs are quarantined (its reports
may still be written if the buffer is executed some other way); STATUS then
carries SEGMENT_QUARANTINE. Its coverage is counted at build time, in the
build's frame. Segments written by another writer (SPU, DMA) and emitters on
other threads stay outside the observed population. Exactly one of RAW_VALID or INCOMPLETE is present;
RAW_VALID cannot coexist with READ_REFUSED or PUBLICATION_UNKNOWN. Unused
kind-specific arguments are zero. A reversed emission bracket sets CLOCK_INVALID.

The publication snapshot is at most UINT32_MAX. A RAW_VALID record needs a
validated completion frontier at least its end serial. UNKNOWN carries zero raw
timestamps (encoded as INCOMPLETE), not a zero-duration interval.
PUBLICATION_UNKNOWN requires a zero snapshot and INCOMPLETE. Later STATUS
publication frontiers cover every earlier emitted snapshot. Label visibility
alone does not release a report pair: its immutable
facts and complete metadata chunk must both be accepted. INCOMPLETE metadata
disposition leaves its physical reports quarantined. A model generation tag is
not physical label reuse proof.

## COVERAGE, type 36

Fixed 352 bytes (384 with the 31/11 masks): schema u16 at 0, flags u16 at 2, instance u32 at 4, owner u32 at
8, reserved u32 at 12; frame id, PPU begin, PPU end, unsupported count and lost
coverage count are u64 at 16, 24, 32, 40 and 48. Bytes 56..63 are reserved. Three
96-byte rows from 64 describe ARRAY, INDEX and CLEAR, each twelve u64 counters:
visited, observed and ten disjoint exclusions in this order: room, report
capacity, metadata capacity, reentry, unsupported arguments, shutdown, ownership
uncertainty, observer disabled, resource lease unverified, epoch invalid.
Flags are TOKEN=1, FINAL=2, SATURATED=4, LOSS=8, OPEN=16 and, with the 15/11
CONFIG masks, FLIP=32 (the row was opened by a flip) and PRESENT=64 (the writer
observed presentation in this row). Without PRESENT every presentation field is
zero and there is no presentation evidence.

Presentation with the 31/11 masks. Only a flip-wait scope is waiting evidence:

| Offset | Type | Field |
|---|---|---|
| 12 | u32 | flip-wait scopes completed inside this frame |
| 56 | u64 | their elapsed owner PPU ticks: flip-wait scope elapsed time, including scheduling and polling |
| 352 | u32 | polling-cadence runs closed by a done poll, every gap within the bound |
| 356 | u32 | broken cadence runs (a gap over the bound, or a flip-wait scope began) |
| 360 | u32 | boundary-spanning cadence runs (rule in the next section) |
| 364 | u32 | boundary-spanning flip-wait scopes |
| 368 | u64 | polling cadence: summed bounded gaps of the complete runs |
| 376 | u32 | flip-wait scopes that returned the timeout error (included at 12) |
| 380 | u32 | reserved, zero |

A flip-wait scope is one `cellGcmWaitFlipComplete(max_us)` call (cell/gcm.h). The
helper polls `cellGcmGetFlipStatus`, sleeps between pending polls
(`sys_timer_usleep`, at most `PS3TC_GCM_WAIT_FLIP_POLL_US` = 100 us each) until the
flip is done or `max_us` of sleep was requested, and returns `CELL_OK` or
`CELL_GCM_ERROR_FAILURE` (the flip is still pending; nothing is cancelled). Its
semantics are waiting for flip completion, so in a PS3_PROF_AUTO build its outer
elapsed time is counted once, in the frame it ran in. That is scope elapsed time
including scheduling and polling, never exact RSX wait or device completion time.
Polls and sleeps inside a scope are part of it and are not counted again.

Polling cadence is every other observed `gcmGetFlipStatus` call
(`cellGcmGetFlipStatus` reaches the same wrapper). A run is a sequence of
consecutive polls that saw the flip pending; its evidence is the sum of the gaps
between consecutive polls (previous return to next entry), counted only while
every gap is at most 1 ms, and the done poll closes the run only when its own gap
is within the bound. A larger gap ends the run as broken (the next pending poll
starts a new one); a flip-wait scope that begins also ends an open run as broken.
The 1 ms bound is below a sixteenth of the shortest display period (16.68 ms at
59.94 Hz), so it is far below any frame a verdict could judge and above the cost of
a poll and a short sleep. Cadence bounds how often the owner looked at the flip,
not whether it was waiting: the owner may have done useful work between any two
polls, and two nonblocking polls around work give the same numbers as a polling
wait. Cadence therefore never establishes pacing; it is named evidence.

Interval ownership. A cadence run or flip-wait scope belongs to the frame it
started in only if it also ends there. When a frame boundary (a flip-derived or
explicit frame opening, an explicit close, the implied closure by a successor, or
owner retirement) falls while a run or scope is open, the interval is charged to
neither frame: each row it touches counts it once at 360 (runs) or 364 (scopes),
the row being closed at the boundary and the row active when the interval ends.
Nothing is clipped or capped into a per-frame share. A frame whose presentation
evidence includes a spanning scope or run reads UNKNOWN ("presentation attribution
unavailable: ...") unless its own complete flip-wait scopes reach the pacing
share. A 15/11 row has no interval ownership at all, which is another reason its
poll interval never yields pacing.

Sleeps and yields outside the helper (`usleep`, `sys_timer_usleep`,
`sys_timer_sleep`, `sys_ppu_thread_yield`) are not recorded: a sleep may serve an
unrelated timer, a yield may return at once, and the flip may have completed
before either, so they could never be waiting evidence. VBlank and flip handlers
and wait-flip GPU commands are not observed either.

With the 15/11 masks, PRESENT rows carry u32@12, the number of flip-status poll
intervals, and u64@56, their owner PPU ticks from the return of the first poll
that saw the flip pending to the entry of the poll that saw it done, charged to
the frame active at the done poll. That interval is an observation, not a wait:
the reader shows it and never derives pacing from it.

The final frame of a capture normally has no closing flip: it is closed at
shutdown as an OPEN row without an end mark, and FRAME_MARK_MISSED is not set
for it. The reader names it "final frame closed at shutdown (no end mark)".

Frames need no profiler call. In a PS3_PROF_AUTO build, rsx/gcm_sys.h routes
gcmSetFlip and gcmSetPrepareFlip (and so cellGcmSetFlip and
cellGcmSetPrepareFlip) through a wrapper that reports each accepted flip on
the owner's attached context. The first such flip opens frame 0; each later
one closes the active frame (end mark, row, block-end clock anchor) and opens
the next through the core frame call, so FRAME events and the 8-frame clock
cadence behave as for explicit frames. Work before the first flip is
unassociated. An explicit ps3dk_prof_frame takes precedence: the first one
closes any flip frame as OPEN, and later flips are ignored (counted). STATUS
records which sources delimited frames. Flips issued another way (a direct
firmware call, cellGcmSetFlipImmediate from a VBlank handler, another
thread) are not boundaries; with no boundary at all, every operation stays
unassociated and the capture has raw RSX ticks only. For a complete
nonsaturated row, visited equals observed plus all exclusions, with checked sums.
Only one final row exists per (instance, owner, frame). Observed counts markers
emitted, not reports recovered or timing qualified.
The unsupported count (offset 40) is the covered emitters the owner thread ran
on a context other than the attached ring and other than an open registered
segment (the observed build announces each segment from `cellGcmSegmentBegin`
to `cellGcmSegmentEnd`; a full segment table leaves it unregistered): a private
buffer. They get no markers, their commands run unchanged, and they are not in
the per-kind rows; with no open frame they count as STATUS unassociated
visits. Emitters inside an open registered segment are covered (SEGMENT
records below). Emitters on other threads and externally written (SPU/DMA)
segments stay outside this count.
Missing association is not assigned to an invented frame. Lost coverage and open
frames stay visible; a bounded queue cannot turn dropped coverage into zero work.

The registered context owner opens coverage at its core frame boundary. Explicit
closure accepts only its active matching token with reserved fields zero; frame
zero is valid. A successor or owner retirement closes an otherwise unclosed row
with OPEN. A reversed PPU clock also leaves OPEN, preserving both observed ticks
rather than substituting a duration. The row's lost count is cumulative coverage
queue loss, and LOSS remains set on later rows after such loss. STATUS reports
the cumulative lost count even if no later coverage row survives.

The owner publishes immutable completed rows into a bounded queue. The drainer
acknowledges each row only after whole-chunk sink acceptance. In-flight owner
rows are never copied by a sealing drainer; a missing owner acknowledgement stays
an explicit lower bound. A draw after explicit closure has no active coverage
frame and increments unassociated visits, even if the core's last frame token
still names the closed frame. Observed counts require both markers to have been
emitted; device readback disposition remains the separate OPS result.

## STATUS, type 37

Fixed 128 bytes: schema u16 at 0, flags u16 at 2, instance u32 at 4. Fifteen u64
fields from offset 8 are: last admitted id, admitted, operations written,
operations dropped, pending unreported, frames written, frames lost, unassociated
visits, internal suppressed, clock attempted, clock written, clock lost, resource
conflicts, last published serial, held report pairs.

Flags are FINAL=1, LOWER_BOUND=2, SATURATED=4, OWNER_UNACK=8,
RESOURCE_CONFLICT=16, CONTEXT_RESET=32 and, when CONFIG covers CALL,
SEGMENT_QUARANTINE=64, FRAME_MARK_MISSED=128, FRAMES_FLIP=256 and
FRAMES_EXPLICIT=512 (both together: flips delimited frames until the first
explicit call). All of these, saturation, conflict and reset stay sticky. Cumulative counters/frontiers never decrease. Pending and held pairs are
gauges and may decrease. Written counters equal accepted chunks. Exact admitted
equals written plus dropped plus pending. Exact FINAL requires both gauges zero;
an unacknowledged owner requires a lower bound. FINAL occurs once and forbids
later records. A live conflict/reset stops admission and quarantines outstanding
resources, without replacing CONFIG or rewriting earlier retained facts.

## CLOCK, type 38

Fixed 96 bytes: schema u16 at 0, flags u16 at 2; instance, epoch, anchor id, role,
slot, serial and polls are u32 at 4, 8, 12, 16, 20, 24 and 28. PPU before, PPU
after, raw timestamp and repeated read are u64 at 32, 40, 48 and 56. First and
second label reads are u32 at 64 and 68. Frame id u64 at 72, owner u32 at 80,
reserved u32 at 84, elapsed timebase ticks u64 at 88 complete the record.

Role zero is a scheduled owner-boundary anchor; ordinal determines hold-out
membership, never the outcome. Flags are VALID=1, TIMEOUT=2, CLOCK_REGRESSION=4
and READ_REFUSED=8, exactly one. Only valid, stable, nonpoison reads with exact
completion identity constrain a map; failed attempts stay visible.

For the first console qualification, the owner samples at frame boundaries
0, 8, 16, 24, 32, 40, 48 and 56, then at explicit frame-63 closure. The same
eight-frame cadence and 64-frame block-end rule continue for the whole capture
while the drainer keeps accepting anchors (slot reuse below). Core FRAME timestamps retain their original position; sampling
runs at the owner hook before coverage opens and application work resumes.
Observer wait time is the CLOCK elapsed field, separate from application waits.
No sampling occurs inside draw hooks, wrap callbacks or a sampler thread.

Each attempt checks room for six words without invoking a reserve callback,
publishes metadata before original PUT, and uses ordered PPU reads with both
a two-second deadline and a 100,000-poll bound. Deadline and regression checks
precede acceptance, including after the repeated report read. A regressed or
future physical label invalidates the instance. Failed anchors retain their
identity and observed brackets; their raw/repeated fields are zero.

Sixteen physical clock reports are reused. Anchor id and serial are one
per-instance counter, nonwrapping and independent of the slot: CLOCK rows can
repeat a report slot, never an id or serial, and UINT32_MAX is never issued.
Only the owner hands a free slot to a new anchor; only the drainer returns it,
after the sink accepted that anchor's whole record, and only when the anchor
completed validly (its exact serial seen twice, stable repeated read) or wrote
no command at all. Any other outcome after a command was written (timeout,
regression, future label, unpublished PUT, unstable or poison read, STOP while
waiting) quarantines its slot for the rest of the instance, so a late write can
only land on a slot nobody reads again. A partial sink write releases nothing.
A label between the last accepted serial and the current one is an earlier
anchor completing late and is not a completion; below or above that range
invalidates the instance. With no free slot, no record room (sixteen records)
or no serial left, the attempt is a capacity loss with no device write, counted
in STATUS clock_lost. Slot state never passes to another instance or capture.

The capture must establish its own bounded map and both H-SPLIT hold-outs. No
nominal timestamp rate or per-application profile row substitutes for missing
clock evidence. The timer, report-before-label/coherent-read, reuse and console
gates remain separate from host encoding and lifecycle tests.

Sustained anchor cadence over long gameplay captures is a mandatory stage 2
beta/release gate. The reuse scheme above has host lifetime controls
(tests/sdk/auto-clock-reuse-test.py: thousands of frames, delayed writes,
cumulative labels, slot wrap, slow and refusing sinks, STOP in each
transition). Both ABI builds and a separately reviewed target reuse control are
still required before a sustained-capture timing claim; a unique-slot timer
probe cannot prove reuse. Visible capacity loss does not satisfy that gate; no
extrapolation fills missing support.

## Automatic FIFO attachment lifetime

The selected runtime prepares the FIFO observer during the intercepted GCM
initialization, using the same owner incarnation and RSX domain as AUTO. The
independent THREAD drainer remains required. Application code does not call a
FIFO attach function. A failed observer install produces the refused AUTO
configuration with reason 8; it does not change the application's GCM result.

Preparation holds the AUTO configuration claim while reserving resources and
installing an unpublished FIFO observer. It releases the extension mutex before
FIFO setup. After the AUTO configuration commits, a separate compare/exchange
publishes the FIFO instance. The first accepted wrap boundary freezes the AUTO
configuration using only atomic state and the validated owner TLS. The callback
acquires no mutex and makes no device or PAL call.

Shutdown claims unpublished FIFO state before waiting for owners. If it wins
before AUTO commits, the owner removes the observer, frees its unpublished block
and drops its unpublished lease; the state cannot return from SEALED to NONE.
If AUTO has committed but FIFO has not published, the owner cancels only the
unpublished FIFO block. The AUTO capture and lease remain available to the
sealing drainer, with an unacknowledged owner reported as lower-bound evidence.
An attached owner removes its observer when retiring; the FIFO drainer releases
its block only after accepting the final status. Unacknowledged published state
remains quarantined. The accepted THREADS declaration is retained as a capture
fact even after the core retires or reuses that thread slot.

Host controls cover installation refusal, the two shutdown windows, a foreign
registered thread, six ordered boundary records, override refusal after the
first boundary, and owner retirement. These controls do not establish device
progress or target callback timing. Target, whole-stack, installed consumer and
console gates remain separate.

## Reader: `ps3dk-prof auto [--json] <file>`

`tools/ps3dk-prof/src/auto_rsx.rs` decodes bit 7 and chunks 34 to 38 with the
rules above and refuses the whole file (MALFORMED, rules AutoConfig, AutoOps,
AutoCoverage, AutoStatus, AutoClock) on any violation: unknown variants, flags,
reasons or nonzero reserved fields, AUTO data before CONFIG or after FINAL,
anything but one zero FINAL STATUS after a REFUSED CONFIG, decreasing
operation, anchor or frame order, a second final row, broken row accounting,
and a FINAL STATUS whose written counts differ from the parsed records. A
reader that predates a later OPS flag (for example SEGMENT) refuses it as an
unknown flag rather than misreading it. Retention ceilings (operations, rows,
anchors) stop retention with a named refusal; every wire rule still applies.

The exit status is that of `check`, or 1 when the capture has no AUTO family,
a REFUSED CONFIG (shown as `automatic observation unavailable: <REASON>`) or a
retention refusal.

The capture maps RSX ticks only through its own anchors: every anchor VALID,
no anchor lost to capacity, at least four distinct RSX coordinates, a
nonempty exact affine feasible set with a slope bounded above and away from
zero (the `clockmap` solver), and in both H-SPLIT parities at least one
DECISIVE held-out anchor and no contradiction. Otherwise the map is
`AUTO_CLOCK_UNQUALIFIED` with the missing condition and only raw ticks are
shown. Conversion uses the slope interval and only inside the anchors' RSX
support.

Each COVERAGE row gets one verdict. It is UNKNOWN, with the first missing
input named, unless: the capture has a FINAL status and no resource conflict
or context reset; the row is associated, FINAL, not OPEN, SATURATED or after
loss; no covered operation was excluded and no visit fell outside the observed
population; every observed operation has a RAW_VALID record; the FIFO family
is present and FINAL exact and every wrap tagged with the frame is complete;
and the map is qualified. Every aggregate over a row is checked: the rows are reconciled per kind on the
wire, but totals across kinds (observed, excluded) may still not fit in u64, and
such a row reads UNKNOWN "coverage totals overflow" rather than wrapping. The
retained operation records of the frame are reconciled kind by kind (ARRAY,
INDEX, CLEAR records against each kind's observed count), never only by their total: equal totals over different
kinds are not the population the row describes. A frame with CALL-segment records reads UNKNOWN before this
reconciliation: a segment's operations are counted in the row of the frame that built it and recorded under the
frame that called it, and a record does not carry its building frame, so equal counts can describe different
segments (one built in the frame, another called in it) and an exclusion in the building frame would be lost. With D the row's PPU interval, W the owner's
complete FIFO wrap wait tagged with the frame (lower bound) and S the frame's
RSX reached-marker span (first operation begin to last end) on the PPU clock:

| Verdict | Condition |
|---|---|
| RSX-bound candidate (fetch) | W >= D/4 and S >= D/2 (lower bound of S) |
| pacing | W = 0, S < D/2 (upper bound), and the row's flip-wait scope elapsed time P >= D/4 (31/11 rows only) |
| UNKNOWN: "presentation attribution unavailable: a flip-wait scope spans a frame boundary" (or "... flip-status polling spans a frame boundary") | W = 0 and S < D/2 without such a P, and a spanning scope (or run) touched the row |
| UNKNOWN: "presentation polling observed; waiting not established" | W = 0 and S < D/2 without such a P, with polling cadence (complete or broken runs) or 15/11 poll intervals |
| UNKNOWN: "RSX not limiting; PPU work vs presentation pacing not distinguished" | W = 0 and S < D/2 with no presentation evidence |
| mixed candidate | 0 < W < D/4 and S >= D/2 |
| PPU-bound candidate | never from this family: it needs positive evidence that the owner was busy, not waiting |
| UNKNOWN | anything else, named as unresolved |

The two shares are provisional starting values, printed with every JSON
report, to be tuned against the first console capture. FRAME marks give each
frame's RSX begin and end (raw, and mapped as a duration when the map
qualifies); they are shown but do not yet enter the verdict. Segment records
count toward their call frame. A
verdict is a candidate with its evidence, never utilization or a busy share.
