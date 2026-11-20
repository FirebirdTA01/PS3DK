# Private FIFO rolling restart checkpoint

`fifo::rolling` is a standalone, typed restart adapter for FIFO phase events
(types31–33) and optional per-frame owner accounting (type43). It does not
enable runtime rolling, join other families, load a physical container, provide
live transport, certify completion, or promote RSX frame verdicts. The ordinary
FIFO reader and its default output remain separate.

The adapter keeps cumulative validation facts apart from retained measurements.
A prefix endpoint can validate the next phase, but never becomes a retained
measurement endpoint. Complete phase pairs wholly inside the suffix retain their
original PPU bounds, including inner phase pairs of a boundary-crossing wrap.

## Caller contract and results

`checkpoint`, `advance` and `read` consume original FIFO payloads and their
original chunk indices. `Refs` supplies the actual capture metadata, declared
RSX domains and owner incarnations. Type43 additionally receives the validated
WINDOW state at that original position. Every accepted WINDOW_END must be passed
as an explicit callback, including when its accounting row is prefix-only.

The caller owns original framing/CRC validation, metadata chronology, complete
delivery of FIFO chunks/window callbacks, and the exclusive original next-chunk
frontier. Gaps in the input indices permit intervening other-family chunks;
they do not authorize omitted FIFO input. This adapter makes no combined
original-byte-prefix claim. Export contexts are refused: EXPORT40 selection
exceptions are not rolling restart semantics.

Pre-CONFIG checkpoints can precede CLOCKS, THREADS and domain declarations.
Unknown identities remain unknown. An observed CLOCKS frequency is bound
exactly; CONFIG requires the actual declared owner/domain and matching
frequency. The caller must not substitute future final metadata for an earlier
cut. Ordinary context growth across genuine bootstrap chunks is supported.

The adapter returns its own `Retained` type: prefix, retained and cumulative
census; original chunk frontiers; genuine latest status/final fence; retained
accounting rows and phase views; and prefix-touched frame membership. It does
not return a full `Capture` or a whole-frame zero-wait conclusion. The membership
records are evidence that earlier events for those frames were evicted.
Unassociated prefix events are disclosed separately. A later consumer must
respect this boundary information before interpreting an empty retained wait
list as zero.

## Checkpoint wire schema

This is private checkpoint schema1, magic `PFRFIFO1`. Integers are little endian;
the final four bytes contain IEEE CRC32 over every preceding byte. The header is
64 bytes, and the fixed scalar block ends at160. Optional blocks are in the
order CONFIG, last Event, last STATUS, membership runs. The exact encoded length
is `164 + config_present*64 + event_present*96 + status_present*80 + runs*24`.

| Offset | Field |
|---|---|
| 0 | Eight-byte magic |
| 8,12 | u32 schema1, u32 header size64 |
| 16 | u64 total checkpoint bytes including CRC |
| 24 | u64 exclusive original next-chunk frontier |
| 32 | u64 historical input record-work census |
| 40 | u32 presence/state flags |
| 44 | u32 canonical membership-run count |
| 48,52 | u32 windows-enabled (0/1), reserved zero |
| 56 | u64 observed CLOCKS frequency, zero while unknown |
| 64,72,80,88 | u64 parsed events, ENTER, EXIT and STATUS counts |
| 96 | u64 last valid event `tb_after`, zero when absent |
| 104,112,120 | u64 accounting parsed count, known sequence gaps, last sequence |
| 128,136 | u64 last accounting frame and window |
| 144,152 | u64 ENTER/EXIT census at the saved STATUS |

Flags are CONFIG1, LAST_EVENT2, VALID_TB4, STATUS8, SATURATED16, FINAL32,
ACCOUNT_FRAME64, PREFIX_UNASSOCIATED128 and CLOCK_BOUND256. Other bits refuse.
Absent fields have their canonical zero representation. FRAME0 remains distinct
from an absent accounting frame through ACCOUNT_FRAME.

The optional blocks encode accepted typed FIFO Config, Event and Status using
their existing canonical64/96/80-byte layouts. Import validates their fields
and restores typed state. It does not feed invented events/statuses through the
ordinary chunk dispatcher. Only one last event and one latest status are stored;
the checkpoint never retains the complete prefix event stream.

Each membership run is `(owner:u32, zero:u32, first:u64, last:u64)`. Runs are
sorted, nonempty, disjoint and nonadjacent; adjacent runs merge. Event frame IDs
use their ordinary full u64 range. Type43 retains its separate less-than-2^63
frame/sequence limits. The exact membership may have holes and is not equivalent
to one maximum frame number. Noncanonical runs, cardinality overflow and
membership impossible under the event census refuse.

## Preserved validation

Every suffix event still obeys the ordinary strict sequence/wrap/kind protocol,
same-wrap frozen tail/request/token rules, CONFIG identity and valid-clock
ordering. The last valid clock survives even when the most recent event had an
invalid clock. Prefix event counts and ENTER/EXIT counts are imported once.

Every STATUS validates against prefix-plus-suffix totals. Its last-attempt field
still equals the last **written event sequence**, even when the suffix contains
no events. Provisional LOWER_BOUND, OWNER_UNACK, sticky saturation, high-water,
counter monotonicity and exact-final conservation remain enforced. No FIFO
chunk, including43, may follow the FIFO FINAL fence.

A saved provisional status may precede newer events in the checkpoint. Its
saved ENTER/EXIT census is therefore distinct from the current census and is
validated at that snapshot. Equal event counts require equal phase census;
ENTER and EXIT are mutually exclusive event kinds even under saturation.
These checks do not turn lower bounds into exact source totals.

Type43 preserves its parsed count, last sequence/frame/window and known gaps.
`known_missing = last_sequence - parsed_count`; suffix gaps begin at the saved
sequence, with no allocation proportional to gap size. Storage eviction and an
unknown trailing tail do not become producer gaps. A nonempty windowed cursor
must name a real, nonzero window identity, and a later WINDOW_END still checks
the saved last frame even when no accounting rows remain retained.

Type43 remains a partial-row inventory. Only retained exact rows carry usable
prepared/published/wrap counts; unavailable rows retain their reasons and null
reported values. No cumulative byte equation or complete-frame census is inferred
from FIFO FINAL. Prepared and published counts remain separate: publication can
include words prepared in an earlier frame. These are owner command storage and
publication facts, including profiler commands and the native tail JUMP once;
external CALL target contents and GPU execution are outside their scope.

## Bounds and ancestry

`Limits` independently bounds checkpoint bytes, canonical membership runs,
retained event/accounting rows and actual work for one operation. Input list
counts and lengths are checked before transient event allocation. Prefix
processing clears retained rows after each input; suffix reports retain only
the bounded supplied suffix. Report construction can temporarily duplicate
bounded accounting rows through the shared inventory projection.

Import work is charged for typed blocks and membership runs actually decoded,
not the number of events evicted before the checkpoint. A large historical
census in a small checkpoint remains cheap to import. The historical work field
is canonical accounting with checked arithmetic; it is not a lifetime budget.
`*_with_work` accepts a shared `WorkBudget` so a caller can bound actual work
across a complete family/segment chain without resetting the outer allowance.

Membership fragmentation is an explicit limit: when the canonical exact set
cannot fit, the adapter refuses instead of forgetting affected frames. The
future runtime must preflight that limit before accepting the offending chunk;
this adapter does not implement a resource-stop writer.

CRC and typed consistency checks do not authenticate an arbitrary invented
history. Adjacent checkpoint ancestry requires canonical byte equality between
`advance(previous, actual_inputs)` and the actual next checkpoint. The caller
must also bind original positions to the physical/combined frontier. Changing a
plausible scalar and repairing CRC does not establish that ancestry.

## Acceptance coverage

`tests/rolling_fifo.rs` uses the independent ordinary capture fixture writer and
strict ordinary reader as controls at every original chunk cut. It covers
retained interior phase bounds, crossing-phase censoring, exact advancement,
status arithmetic/fences, saved valid clocks,43 sequence/window checks,
bootstrap context growth, sparse membership, repaired-CRC impossibilities,
large historical census and shared actual-work budgets. Additional audit cases
cover maximum event-frame IDs and impossible imported status/window cursors.
These host tests establish the standalone adapter contract, not console
performance, full-family rolling support or runtime availability.
