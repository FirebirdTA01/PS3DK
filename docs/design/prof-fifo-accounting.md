# Per-frame owner FIFO accounting

The AUTO provider records owner FIFO command bytes prepared and published, plus
completed native wraps, in type 43 records. These are CPU-side storage and
publication observations. They do not measure GPU-executed bytes, GPU utilization,
or GPU frame boundaries. Existing type31–33 wrap-phase records retain their
format, counters and meaning.

## Supported source and boundaries

Accounting applies to the attached AUTO context using the observed native
`ps3tc_fifo_wrap_callback`, its registered contiguous ring, the native publication
seam, and the context's registered owner thread. The manual FIFO-only provider
continues to record wrap phases without claiming ordinary publication coverage.

At attachment the observer establishes a linear current/PUT baseline. At each
owner frame boundary it samples current and checks that actual PUT agrees with
the known publication frontier. The core snapshots the owner/window identity
before and after the existing window transition under the existing window lock;
it releases that lock before querying GCM or invoking accounting. A foreign frame
thread cannot drive the attached context's accounting.

The frame accounting boundary precedes AUTO's frame and clock command emission.
Consequently all commands appended after that boundary belong to the new owner
interval, including an AUTO command that marks the preceding frame's RSX end.
Explicit AUTO frame end samples after its end marker. This is a precisely defined
owner association, not a claim that all commands execute during that frame.

Prepared counts include all command words written in the observed owner ring:
application commands, ordinary CALL packets, profiler commands, and the native
tail JUMP. External CALL target contents are excluded. Publication has a separate
frontier and counts bytes newly exposed by the observed PUT operations. A frame
may publish bytes prepared in an earlier frame, so its published count may exceed
its prepared count. Repeated publication of one frontier adds zero bytes.

Ordinary `FlushBuffer` observation receives the exact IO offset written to PUT
after the existing barrier and AUTO publication hook. An AUTO report serial is
never interpreted as a FIFO offset. The existing report flush/pre-wrap hooks are
independent and retain their ordering and behavior.

Default non-AUTO providers compile without the added publication, callback and
frame hooks. The observed FIFO-only provider does not enable this accounting.
AUTO hooks for an unattached context return without reading current or PUT.

The frame hook acquires the attached state before reading published context
metadata. Only the registered owner can then access the accounting storage;
foreign threads cannot race owner retirement through that pointer. The native
end-identity accessor likewise checks the attached owner before reading AUTO's
capture. This branch accepts the registered physical end. Report-ring integration
may additionally accept its exact active reserved-tail end through that accessor;
it must retain the context, owner and original physical-end checks. An arbitrary
lowered end is not evidence of a known tail.

## Exact native wraps

The native protocol provides six ordered observation points. Accounting requires
all of them and a stable begin/tail pair:

1. WRAP_ENTER accounts the actual prepared distance to the tail.
2. PHASE1_BEGIN observes the already stored four-byte JUMP. It adds those four
   prepared bytes, while PUT at tail publishes only the preceding commands.
3. PHASE1_END preserves the phase sequence.
4. PHASE2_BEGIN observes PUT at begin, publishing the JUMP exactly once.
5. PHASE2_END preserves the phase sequence.
6. WRAP_EXIT establishes the next linear lap base and increments completed wraps.

An early tail does not charge the unused space to the ring end. For example, two
wraps after 64 and 32 command bytes, followed by 16 more prepared bytes with 8 of
those published, produce 120 prepared bytes and 112 published bytes: each wrap
adds one four-byte JUMP. No modular cursor subtraction guesses how many laps
occurred.

## Unavailable evidence

Missing or out-of-order wrap boundaries, suppressed reentrant observation,
backward/out-of-range current, unproven publication, reset, callback replacement,
and counter saturation prevent an exact row. Both native reserve callback entry
paths check context identity before invoking a replacement callback, so restoring
the native callback afterward does not restore lost evidence. A normal CALL
packet does not invalidate owner-buffer progress.

Raw application writes to current/PUT outside the supported seams are not a
fully observed source. An unexpected PUT at a frame boundary invalidates the
accounting; arbitrary unseen mutations must not be described as covered. The
implementation never converts an unknown cursor transition into a known lap.

Partial or adopted frames without matching owner and window boundaries are
unavailable. Unavailable rows carry reason bits and no numeric byte/wrap result.
An active frame at owner detach is attempted as an unavailable tail row. A seal
without owner acknowledgement cannot establish the final tail. A missing row is
unavailable, never a zero-work frame.

## Type 43 wire record

Type 43 requires the existing FIFO file flag and a preceding FIFO_CONFIG. Its
schema 1 payload is 80 bytes, little-endian:

| Offset | Width | Field |
|---:|---:|---|
| 0 | 2 | schema, 1 |
| 2 | 2 | flags: COMPLETE=1, INCLUDES_PROFILER=2 |
| 4 | 4 | FIFO instance |
| 8 | 4 | FIFO generation |
| 12 | 4 | RSX domain |
| 16 | 4 | owner incarnation |
| 20 | 4 | unavailable reason bits |
| 24 | 8 | owner frame ID |
| 32 | 8 | capture window ID; 0 only without explicit capture windows |
| 40 | 8 | prepared bytes |
| 48 | 8 | published bytes |
| 56 | 8 | completed native wraps |
| 64 | 8 | attempted record sequence |
| 72 | 8 | reserved, 0 |

Reason bits are 1 unknown owner flow, 2 missing boundary, 4 token mismatch,
8 window mismatch, 16 saturated counter, and 32 invalid baseline offsets. Other
bits are rejected. Exact rows have flags 3 and reasons 0. Unavailable rows have
flags 2, nonzero reasons, and zero on-wire numeric counters; the JSON result uses
null for those unavailable values.

Instance, generation, domain and owner must match CONFIG. Record sequences and
owner frame IDs strictly increase; sequences are nonzero and below 2^63. Byte
counts are word-aligned and each completed wrap requires at least four prepared
and published bytes. Reserved fields and unknown flags are rejected. Windowed
rows require a matching declared window, and a later WINDOW_END cannot exclude
an already observed accounting frame. These checks also run when retention
budgets prevent keeping the row.

## Bounded transport, retention and compatibility

The producer has a 16-row SPSC queue inside the existing charged 4096-byte FIFO
control allocation. A static assertion enforces this bound. The accounting core
and publication hooks allocate nothing, take no lock and invoke no sink or SDK
function. The frame hook's separate owner/window identity query uses the existing
window lock as described above.
The owner releases completed rows to the drainer; the drainer releases a slot
only after the sink accepts its entire type 43 chunk. A failed sink is terminal.

The attempted record sequence advances before admission. A full queue drops the
newest row without blocking, preserving gaps in later admitted sequences. The
reader counts known gaps with scalar arithmetic and does not allocate for their
size. Trailing drops and frames without rows remain unavailable.

The accounting inventory is always `PARTIAL_ROWS`. A phase FIFO_FINAL does not
turn it into a complete accounting inventory. Counts and any downstream sums
apply only to retained rows; absent frames and unknown tails cannot contribute
zeroes. Row retention shares the bounded FIFO admission budget. Exceeding it
produces a named refusal while structural validation continues.

Old captures without type 43 keep their prior JSON shape and report prepared and
published bytes as not recorded. New captures add `owner_accounting` only when
type 43 rows were parsed. The current capture-selection exporter omits type 43, so
exported selections expose accounting as unavailable. Future export support must
distinguish deliberate selection omissions from producer queue loss.

Host tests cover exact multiple wraps, partial/repeated publication, publication
of prior-frame bytes, unavailable boundaries/windows/owners, saturation, actual
AUTO-injected commands, bounded queue loss, sink rejection, malformed records and
validation after retention refusal. Host models do not establish GPU execution,
target overhead, PPU ABI correctness or console behavior; those require the
separate target checks.
