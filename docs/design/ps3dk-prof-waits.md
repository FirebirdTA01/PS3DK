# Automatic wait observations: schema 1

This extension records elapsed time around selected SDK API calls. It does not
prove that a thread slept in the kernel, identify the thread that woke it, or
measure GPU execution. Raw object handles are not object lifetimes: reuse of the
same numeric handle never creates a dependency edge. Every target in this first
slice remains UNKNOWN. No manual zone is needed at an observed call boundary.

The initial operations are pthread mutex lock, timed mutex lock, and condition
wait/timedwait. Resolution of the kernel object and calculation of the timeout
happen before the interval. The interval includes scheduling delay. The original
raw kernel result and translated API result are kept separately. A timeout or
other returned error still has an observed elapsed interval; it is not evidence
of successful synchronization.

For timed mutex calls, the initial `sysMutexTryLock` and its immediate-success
return are outside the observation interval. The adapter records only the
subsequent kernel wait after a busy result and valid deadline. Immediate timed
success, trylock errors, and pre-wait validation failures are uncovered paths,
not dropped wait records. The operation label does not imply all invocations of
that API were recorded.

The reader and diagnostic command implement the contract below. Runtime adapters,
observed archive installation, the viewer page and target cost measurements are
separate gates. This document does not claim those gates have passed.

## Capability and scope

File flag bit 4 (value 16) enables chunk types 22, 23 and 24. It has no dependency
on domain extensions or a PROFILE. Existing flags retain their prerequisites.
At this implementation base the allowed complete and partial header masks are
0, 1, 3, 7, 16, 17, 19 and 23. Bit 3 and chunk types 19 through 21 belong to the
separate frame-checkpoint work and are not allocated by this extension.

Without bit 4, types 22 through 24 remain unknown chunks. Chunk length, full
payload availability, CRC and chunk sequence are checked before wait semantics.
The existing HEADER, CLOCKS, PROFILE and post-TRAILER rules retain their order.
Wait records do not relax any legacy source, frame, prefix or final-entry rule.

A whole WAIT_RECORDS chunk with a positive record count is event data for prefix acceptance, like a whole
PPU/SPU/GPU event chunk. The remaining prefix checks still apply. A torn wait
chunk contributes no records. An earlier complete chunk is retained.

All integers below are little-endian. Unknown schemas, operations, record kinds,
flags and nonzero reserved fields refuse. Actor identity is the PPU thread
incarnation from an already accepted THREADS entry, not a recyclable slot number.
Wait slot equals successful core incarnation minus one for incarnations 1 through
max_actors. Registration consumes a lifetime slot even if no wait is observed;
slots are never reused. Later declared incarnations are capacity-refused,
derived from THREADS and CONFIG with their names and unknown call counts.
They need no per-thread runtime refusal registry. Truly unregistered calls are capture
aggregate counts; their thread names are unavailable.

## WAIT_CONFIG (22)

Exactly one config precedes the first records or status chunk. Its payload is
56 bytes, with no variable tail:

| Offset | Type | Meaning |
| --- | --- | --- |
| 0 | u16 | Schema, 1 |
| 2 | u16 | Reserved, 0 |
| 4 | u32 | Maximum lifetime record producers, 1..HEADER.max_threads (at most 1024) |
| 8 | u32 | Ring records per producer, power of two in 8..4096 |
| 12 | u32 | Configured allocation ceiling, at most 8 MiB |
| 16 | u32 | Actual wait block bytes |
| 20 | u32 | Record width, 96 |
| 24 | u64 | Nonzero timebase frequency, equal to CLOCKS |
| 32 | u64 | Covered-operation mask, 7 |
| 40 | u32 | Added SDK writable static payload bytes, outside the wait block |
| 44 | u32 | Added SDK TLS symbol payload bytes per thread, at most 64 |
| 48 | u32 | Nonzero static observation path stack bound (scope below) |
| 52 | u32 | Reserved, 0 |

The allocation formula is `4096 + max_actors * (256 + ring_records * 96)`;
it must fit the configured ceiling and equal the actual-byte field. Defaults
64/128 give 806912 bytes under a 1 MiB ceiling. This is not total process memory:
base profiler storage, static storage, TLS across all application threads and
stack are separate. Independent parameter maxima need not fit together.
Static and TLS payload figures account for the SDK's added storage, not linker
padding or the loader's total allocation. The target packet separately reports
OFF/ON ELF TLS template sizes and alignment for its controlled consumers. Those
layout-dependent deltas are not a bound for arbitrary application TLS layouts.
The PC report names capacity-refused threads and directs users to raise max_actors
within core max_threads and max_bytes before initialization. Thread-pool recreation
consumes new incarnations even when concurrent thread count remains small.

The runtime must supply build-audited footprint values. The observation path
stack bound covers the adapter's observed-call subtree, BEGIN/RETURN, reachable
libc functions and syscall wrappers, plus the still-live public/helper frames.
It excludes firmware/kernel frames, the resolver/deadline work preceding the
observation boundary, registration/cleanup, application callers and asynchronous
frames. Whole public-call stack is UNKNOWN: this field is not a thread stack
requirement. A stack-paint high-water observation is a separate measurement,
never a bound. A guessed bound or zero-as-unknown is not permitted in schema 1.
Test fixtures use synthetic values to exercise validation and do not constitute
target footprint evidence. The target packet retains the separate frame
inventories and linked-library identities, including cold-path libc calls.

### Explicit library selection

Observed builds use `librt_profwait.a` plus one of `libps3dkprof_wait.a`
(manual lifecycle), `libps3prof_wait.a` (automatic lifecycle), or
`libps3dkprof_ext_wait.a` (extension runtime with matching live members).
Both pointer models have distinct archives. The default runtime and profiler
archive names are not replaced.

The runtime builder's `--waits-only` entry takes explicit compiler, SDK, output
and optional installation paths. The profiler Makefile's `waits` and
`install-waits` targets require audited stack, TLS and static-payload inputs;
the extension mode has separate static-payload inputs because its struct padding
differs. Each generated variant manifest lists `required_artifacts`, archive and
member hashes, header hashes, source base, build flags and footprint scope.
These artifacts are required only for the selected observed variant, not for an
ordinary SDK build. Installed default archives remain outside these targets.

CMake consumers select `PS3_PROF_WAITS=MANUAL`, `AUTO` or `EXT` before `project()`;
`OFF` retains the default selection. `PS3_PROF_WAITS_ROOT` may name a separate
prefix containing the variants and wait headers. CMake verifies archive/header
identity, selects the observed runtime for both explicit and compiler-injected
`-lrt`, and checks the actual GNU link map before post-link packaging. A missing
artifact, incompatible pointer model, foreign profiler definition or mixed
default/observed runtime is refused. This integration supports executable targets
with Ninja or Makefiles and CMake 3.21 or newer; it does not certify another
build system's library selection.

## WAIT_RECORDS (23)

Prefix: u16 schema 1, u16 reserved 0, u32 record count, followed by exactly
`count * 96` bytes. There is no padding.

| Offset | Type | Meaning |
| --- | --- | --- |
| 0 | u32 | Actor incarnation, nonzero |
| 4 | u16 | Operation: 1 mutex lock, 2 timed mutex lock, 3 condition wait |
| 6 | u8 | Kind: 1 BEGIN, 2 RETURN |
| 7 | u8 | Reserved, 0 |
| 8 | u64 | Attempted record sequence, 1..UINT64_MAX-1 |
| 16 | u64 | Call ID: BEGIN sequence |
| 24 | u64 | PPU timebase |
| 32 | u64 | Raw resolved target handle |
| 40 | u64 | Raw associated mutex for operation 3; otherwise 0 |
| 48 | u64 | Timeout passed to the kernel; operation 1 requires 0 |
| 56 | u64 | Application site; 0 if unavailable |
| 64 | u32 | Frame owner, 0 in schema 1 |
| 68 | u32 | Flags: SITE_UNKNOWN=1, FRAME_UNKNOWN=2 |
| 72 | u64 | Frame ID, 0 in schema 1 |
| 80 | i32 | Raw kernel result; 0 in BEGIN |
| 84 | i32 | API result; 0 in BEGIN |
| 88 | u64 | Reserved, 0 |

FRAME_UNKNOWN is mandatory; SITE_UNKNOWN is set exactly when site is zero.
RETURN repeats the BEGIN identity and arguments, with a greater record sequence.
Sequences increase per actor and include dropped attempted records. Call IDs
never repeat. There is at most one active call per actor; nested instrumentation
is refused and counted by the runtime, not represented as nested records.

Only admitted endpoints consume sequence numbers. BEGIN is odd, RETURN is even,
and RETURN's call ID equals its sequence minus one. Admission, nested and reentry
refusals do not consume sequence numbers. A lost BEGIN publication still returns
a live cookie: its RETURN is attempted and may independently be lost. STOP may
leave the final BEGIN open with no later record. These rules apply independently
per actor; threads may interleave freely in the file. They do not turn an orphan
RETURN into a pair or supply its missing timestamp.

A missing RETURN leaves an OPEN observation. A RETURN whose BEGIN is lost is
an ORPHAN RETURN and has no duration. Its BEGIN must fit in the current sequence
gap, after the last observed record. If an older call was open, there must also
be a missing-record position for that older call's END before the new BEGIN.
Gaps inside an earlier observed call cannot justify a later orphan's BEGIN.
An old call cannot return after a newer call has begun. These constraints reject
impossible ordering without inventing the contents of a gap.

A matched pair with nondecreasing timestamps has exact integer elapsed ticks.
A regressed timestamp yields CLOCK REGRESSION with unknown elapsed time, never
unsigned underflow or zero. Missing endpoints likewise remain unknown.

## WAIT_STATUS (24)

Prefix: u16 schema 1, u16 reserved 0, u32 actor count. One capture summary and
then exactly that many actor summaries follow. Each summary is 64 bytes:

| Offset | Type | Meaning |
| --- | --- | --- |
| 0 | u32 | Actor incarnation; 0 for capture aggregate |
| 4 | u32 | LOWER_BOUND=1, SATURATED=2, ACTOR_UNACKNOWLEDGED=4 |
| 8 | u64 | Last fully written record sequence, 0 if none |
| 16 | u64 | Exact written record count |
| 24 | u64 | Dropped attempted records |
| 32 | u64 | Refused BEGIN admissions, once per refused call |
| 40 | u64 | Actor: nested/reentry refusals; aggregate: profiler-internal exclusions |
| 48 | u64 | Unregistered refusals, capture aggregate only |
| 56 | u64 | Attempted record sequence, or 0 when unacknowledged |

SATURATED or ACTOR_UNACKNOWLEDGED requires LOWER_BOUND. For an acknowledged,
unsaturated actor, written + dropped equals attempted, which is at least the
last written sequence and below UINT64_MAX. Written count and last written
sequence always match the accepted record inventory, even for lower bounds.
Saturation disables exact reconciliation. Refusals and dropped records have
different units and must not be added as if they counted the same thing.

Capture aggregate uses only flags (LOWER_BOUND/SATURATED), refused,
internal-excluded (offset 40) and unregistered fields. All others are zero.
Internal and unregistered are disjoint subsets: their sum cannot exceed refused.
The profiler PAL marks its mutex acquisition scope, restoring the previous guard
afterward. This scope, drainer/sink callbacks and GPU-profiler callbacks are
excluded before actor admission, including pre-registration. They produce no
application rows or sequence numbers. Their capture-level count is explicitly
named `internal_excluded` in JSON and the viewer; per-actor nested/reentry counts
retain their existing meaning. No new chunk or field width is allocated.
Status contains every recorded actor exactly once and may additionally name
eligible declared actors with no records. Actors above max_actors have no storage
and cannot appear in records or per-actor status; their named refusal is derived
as described above. No wait chunk follows status. There is at
most one status chunk. Its absence is incomplete wait coverage, not evidence
of zero calls; it does not by itself make a cut prefix malformed.

## Reader budgets and presentation

Default per-pass budgets: 10 million accounted operations, 32 MiB accounted
retention and 262144 calls. The parser charges before decoding records, tree
searches and allocation. It retains one call entry per identity and no list of
every missing sequence. Accounted allocation allowances are 512 bytes per call,
256 per actor and 64 per config or summary. These are bounds on the chosen
representation plus allocation allowances, not a measured system allocator RSS.

Budget refusal is sticky and visible. Subsequent wait chunk envelopes still
receive length/schema/ordering checks; deep wait semantics are not claimed after
refusal. Legacy chunks, CRC and chunk sequence continue to be checked. A refused
wait inventory cannot certify coverage or dependencies.

Analysis precharges actor/name visits and output arrays, then visits each stored
call once. Rows borrow names and records. Text/JSON output streams without a
second output-sized tree. Its operation allowance includes string escaping.
Parsing the enclosing file and legacy source analysis have their existing
budgets; this wait-specific budget is not a whole-process or whole-file bound.

`ps3dk-prof waits FILE` prints observations and named coverage.
`ps3dk-prof waits --json FILE` exports deterministic schema-1 JSON with integer
ticks and frequency. Missing elapsed time or results are null, not zero.
The command keeps capture validity exit codes and returns 1 on wait-budget
refusal. The usual check/report commands additionally show wait coverage when
the capability is present; captures without it keep their existing output.

RECORDED denotes receipt of the status bytes, not complete instrumentation,
acknowledgment of every actor, absence of loss, or file durability. Coverage
explicitly excludes raw syscalls, semaphore, lightweight mutex, GPU and SPURS
adapters in this slice. No API interval is labelled blocked-CPU time. No raw
handle equality is labelled MATCHED. Frame membership is unknown here and is
not inferred from adjacency.

The viewer's **Waits** page builds its model once per immutable document. A new
file or live snapshot gets a new model. It groups retained calls by operation
and raw caller site, with separate retained-call and elapsed-sample counts,
minimum/maximum elapsed, errors, open calls, orphan returns and regressed clocks.
There is no summed elapsed column across actors. Missing values remain UNKNOWN.
Coverage lists declared thread names, lifetime capacity refusals and recorded
counter qualifications; absent status is PROVISIONAL. The page reports added
SDK static/TLS payload and the observation-path stack bound, with the whole
pthread stack UNKNOWN. Long rows expose their full text on hover.

The owned view has a separate default allowance of 20 million work units and
64 MiB of accounted peak retention, including the borrowed reader report and
temporary grouping storage. It precharges from row/actor counts before copying
records or building groups. Each scrolling list paints at most 256 visible
rows per repaint and states its visible/omitted population. These allowances
are not a whole-viewer process memory bound.
