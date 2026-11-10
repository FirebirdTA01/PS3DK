# FIFO wrap phase events: schema 1 (slice 1)

The GCM FIFO wrap retires a lap in two phases (`sdk/libgcm_cmd/src/ps3tc_fifo_wrap_protocol.h`):
phase 1 publishes the lap up to the tail JUMP (PUT = tail) and waits for GET == tail; phase 2
releases the JUMP (PUT = begin) and waits for GET == begin. A long wrap holds the thread that owns
the GCM context. This extension records, on that owner thread, one event at each of six protocol
boundaries, so a capture shows which phase held the PPU, for how long, and where the record is
open or lost.

Slice 1 is phase events only. There is no GET/PUT/REF sampling, no completion report, no extra FIFO
word, report slot or label, and the wrap protocol itself is unchanged: the same tail JUMP and PUT
stores, the same barriers, the same equality waits, pause and sleep cadence, and the same reserve
return contract. The observer cannot change the protocol's outcome (its callback returns nothing).

What the record is: PPU timebase wall time of the owner thread between protocol boundaries,
including scheduling, the protocol's own sleeps and the observer's cost. What it is not: GPU busy or
idle time, RSX execution time, or the time a frame took on the RSX. A frame token on an event is
the owner thread's current profiler frame when the wrap began: an association with the owner, never
evidence of which frame the GPU was executing.

## Capability and wire identity

File flag bit 6 (value 64) enables chunk types 31, 32 and 33. It requires bit 0 (domain
extensions): a FIFO instance names a declared RSX_TIMESTAMP domain. It has no dependency on PROFILE
(bit 1), SPU order (bit 2), frame checkpoints (bit 3), waits (bit 4) or function paths (bit 5), and
their prerequisites are unchanged. Without bit 6, types 31 through 33 remain unknown chunks. Bit 6
without bit 0 refuses, in a whole header and in a header cut after byte 12. A reader that does not
know bit 6 refuses the flag. Default archives never set it.

| Type | Name | Count |
| --- | --- | --- |
| 31 | FIFO_CONFIG | exactly one per capture (one instance in v1), before any FIFO_EVENTS or FIFO_STATUS |
| 32 | FIFO_EVENTS | any number; each holds whole events |
| 33 | FIFO_STATUS | any number of provisional ones, at most one FINAL, nothing FIFO after the FINAL |

All integers are little-endian with exact lengths. Unknown schemas, unknown feature or flag bits
and nonzero reserved fields refuse. A chunk is accepted whole or not at all; a torn chunk
contributes nothing.

## FIFO_CONFIG (31), 64 bytes

| Offset | Type | Meaning |
| --- | --- | --- |
| 0 | u16 | schema = 1 |
| 2 | u16 | features: bit 0 PHASE_EVENTS; slice 1 accepts exactly 1 |
| 4 | u32 | instance (nonzero; 1 in v1) |
| 8 | u32 | domain: a declared RSX_TIMESTAMP domain id |
| 12 | u32 | owner incarnation: a declared THREADS incarnation (the context owner) |
| 16 | u32 | record bytes = 96 |
| 20 | u32 | ring records (16 to 65536) |
| 24 | u32 | max bytes: the configured ceiling |
| 28 | u32 | allocated bytes = ring records x 96 + 4096 (control allowance), at most max bytes |
| 32 | u64 | timebase frequency: the CLOCKS frequency |
| 40 | u32 | ring begin offset: IO offset of the context's begin, 4-aligned |
| 44 | u32 | ring end offset: IO offset of the context's end (the reserved JUMP word), 4-aligned, above begin, below 2^29 |
| 48 | u32 | generation (nonzero; 1 in v1) |
| 52 | u32 | reserved = 0 |
| 56 | u64 | reserved = 0 |

The domain and the owner must be declared before the CONFIG (the writer waits for both
declarations to be accepted). The ring offsets were validated once on the owner at attach; the
reader uses them only to bound event offsets, never for modular pointer arithmetic.

## FIFO_EVENTS (32)

A 16-byte prefix: u16 schema = 1, u16 reserved = 0, u32 instance, u32 count (at least 1), u32
reserved = 0; then `count` events of 96 bytes. The length is exactly 16 + 96 count and at most one
chunk payload.

| Offset | Type | Meaning |
| --- | --- | --- |
| 0 | u64 | attempt sequence |
| 8 | u64 | wrap id |
| 16 | u64 | tb before |
| 24 | u64 | tb after |
| 32 | u32 | owner incarnation (= CONFIG) |
| 36 | u16 | kind |
| 38 | u16 | flags: bit 0 TOKEN_PRESENT, bit 1 CLOCK_INVALID |
| 40 | u32 | frame owner (token) |
| 44 | u32 | reserved = 0 |
| 48 | u64 | frame id (token) |
| 56 | u32 | begin offset (= CONFIG ring begin) |
| 60 | u32 | tail offset: the JUMP word, above begin, at most ring end, 4-aligned |
| 64 | u32 | target offset |
| 68 | u32 | requested words: the reserve that caused the wrap, at most the ring's words |
| 72 | u32 | current offset |
| 76 | u32 | reserved = 0 |
| 80 | u64 | reserved = 0 |
| 88 | u64 | reserved = 0 |

Kinds and their boundaries, with the timebase bracket each one carries:

| Kind | Name | Bracket | target | current |
| --- | --- | --- | --- | --- |
| 1 | WRAP_ENTER | one read after the callback's checks, before the tail JUMP store (before = after) | 0 | tail |
| 2 | PHASE1_BEGIN | before: just before the PUT = tail store; after: after the barrier that follows it | tail | tail |
| 3 | PHASE1_END | before: after the last wait iteration (or PHASE1_BEGIN's after, with no iteration); after: once the GET == tail check succeeded | tail | tail |
| 4 | PHASE2_BEGIN | as kind 2, around PUT = begin and its barrier | begin | tail |
| 5 | PHASE2_END | as kind 3, for GET == begin | begin | tail |
| 6 | WRAP_EXIT | one read after current was rewound to begin (before = after) | 0 | begin |

The successful equality check of a phase END happened inside its bracket; the PUT store of a phase
BEGIN happened inside its bracket. No bracket claims the exact store or fetch time. The wrap's
existing statistics (`ps3tc_fifo_wrap_stats`) time the protocol between ENTER and EXIT and are not
changed; the observer's cost inside the phases is included in both.

Rules, per event: attempt sequence 1 to 2^63-1, strictly increasing across the capture; wrap id at
least 1; kind 1 to 6; no unknown flag; reserved fields 0; owner incarnation, begin offset, target
and current as above. TOKEN_PRESENT means frame owner = the CONFIG owner (frame id 0 is valid);
without it both token fields are 0. The token is frozen at WRAP_ENTER: every event of a wrap carries
the same token, tail offset and requested words. Without CLOCK_INVALID, before <= after and before
is not below the previous valid event's after. With CLOCK_INVALID (the owner saw its clock regress)
the raw values are kept and the event gives no elapsed time.

Ordering: (wrap id, kind) strictly increases. When two visible events have consecutive attempt
sequences, the second is the exact successor of the first (the next kind of the same wrap, or
WRAP_ENTER of the next wrap id after WRAP_EXIT). A sequence gap means the attempts between were
dropped: no pairing crosses it.

### Timestamp ordering and cost

In the observed GCM build every bracket endpoint comes from one helper, `ps3tc_fifo_tb`, which is
`sync; mftb; isync` in a single volatile asm with a memory clobber. Compiled with GCC 16.1.0 at
`-O2 -mcpu=cell` it is `hwsync; mftb r3; isync; blr` in both data models
(`tests/sdk/fifo-tb-order-test.sh` checks that sequence statically; it is not a hardware ordering
test). The claim is instruction-execution order on the issuing PPU, as described in the public IBM
Cell Broadband Engine Programming Handbook v1.1, sections 20.1.2.1 and 20.1.2.4: the full `sync`
orders the execution of preceding instructions before the timebase read, the trailing `isync`
keeps subsequent instructions from executing ahead of the completed read, and the memory clobber
stops the compiler from moving memory accesses across it. It is NOT device completion: the
handbook distinguishes this ordering from the physical completion of preceding storage accesses,
so a stamp proves neither RSX completion, nor an external device's acknowledgement, nor GPU idle.
`lwsync` and `eieio` are not substitutes across the cacheable command memory and the uncached
control register. The original protocol's stores, barriers and waits are unchanged.

The END bracket's lower endpoint is re-read after every failed wait iteration, before the next GET
check (no event is emitted per iteration), so the bracket surrounds the issuing PPU's final,
successful GET observation. Without that read the lower endpoint would stay at BEGIN's upper
endpoint and could span the whole wait. The bracket is about the PPU's observation, not the
instant the RSX first reached the target; a pause or scheduling delay can still widen it.

Cost: a completed observed wrap with N1 and N2 failed iterations in its two phases makes
`8 + N1 + N2` timestamp reads (the two wrap endpoints, three reads per phase, and one per failed
iteration), and each read adds one `sync` and one `isync`: `2 x (8 + N1 + N2)` barrier instructions
beside the protocol's own three. The handbook (section 10.7.2.10) describes both barriers as
stalling both PPE threads, and `isync` also discards the issuing thread's prefetched
instructions; their cost depends on outstanding work, so no fixed cycle, nanosecond or upper-bound
cost is claimed. Recorded intervals include this observer cost; nothing is subtracted. A default
(unobserved) build makes none of these reads.

## FIFO_STATUS (33), 80 bytes

u16 schema = 1, u16 flags, u32 instance, then nine u64: attempted, written, dropped, refused, last
attempt sequence, wraps started, wraps completed, open wrap id, high water.

Flags: bit 0 FINAL, bit 1 LOWER_BOUND, bit 2 SATURATED, bit 3 OWNER_UNACK.

- `attempted`: attempt sequences assigned (each admitted boundary, recorded or dropped).
- `written`: events in FIFO_EVENTS chunks the sink accepted so far; always equal to the events the
  reader has parsed before this status.
- `dropped`: admitted attempts not recorded because the ring was full.
- `refused`: boundaries not admitted (instance stopped or sealed, stop latch set, or the sequence
  ceiling); they have no sequence.
- `last attempt sequence`: the sequence of the last written event (0 if none).
- `wraps started` / `wraps completed`: WRAP_ENTER and WRAP_EXIT boundaries the owner reached,
  recorded or not. Entered and completed counts are separate; a started wrap without a completion
  is open, never a zero-length wrap.
- `open wrap id`: the wrap the owner was inside, or 0.
- `high water`: the largest ring occupancy the owner saw (at most ring records).

A provisional status (FINAL clear) and any status for an owner that did not acknowledge (it never
detached before the capture was sealed: OWNER_UNACK) carry LOWER_BOUND: the counters were read with
acquire loads from a live owner and are lower bounds, consistent only as `attempted >= written +
dropped`. OWNER_UNACK declares the owner's final totals unavailable: it does not certify that no
later activity happened. The exact FINAL status (FINAL without LOWER_BOUND) follows the owner's
detach: then `attempted = written + dropped`, `wraps started - wraps completed` is 0 or 1, and
`open wrap id` is `wraps started` when it is 1, else 0. SATURATED is sticky; with it the equalities
become lower bounds. Counters never decrease from one status to the next.

## Runtime

The observed archives are explicit build goals (`fifo` in `sdk/libgcm_cmd/Makefile` and
`fifo-ext` in `sdk/libps3dkprof/fifo.mk`); the default archives and their build plans are
unchanged. An application links `libgcm_cmd_prof_fifo.a` in place of `libgcm_cmd.a` and
`libps3dkprof_ext_fifo.a` in place of `libps3dkprof_ext.a`. Linking the FIFO extension with the
default GCM archive fails at link time (the observer bridge symbols are only in the observed GCM
archive).

`ps3dk_prof_fifo_attach(gpu, ctx, options, &instance)` (`<ps3dk/prof_fifo.h>`), on the context
owner, with the context quiescent:

- EBUSY inside a drain, sink or GPU call; ENOTINIT; ENOTSUP without GPU domains;
- EPERM unless the capture runs the background drain thread (THREAD mode): APP-owner draining is
  refused, because an owner parked in a wrap could never drain its own events;
- EINVAL for a bad size, reserved field, ring records outside 16 to 65536, a ceiling outside 5632
  bytes to 16 MiB, or ring x 96 + 4096 above the ceiling (checked before any allocation), for a
  domain this thread did not attach or another context, and when the GCM bridge refuses the
  context;
- ESTOPPED after the shutdown claim; ETHREAD for an unregistered owner thread; EFULL for a second
  attach in the same capture (v1 has one instance per capture, never reattached); ENOMEM.

Admission and the seal are linearized by one claim on the instance state. The seal, first in its
path and before any sink check, turns an unattached or still-attaching instance into SEALED by
compare-and-swap. Attach claims the instance (NONE to ATTACHING) only after its validation,
re-checks the shutdown claim and stop latch after winning it, and publishes with ATTACHING to
ATTACHED. An attach that loses the first claim, sees the stop, or loses the publication returns
ESTOPPED with instance 0, its observer removed and its block freed (the seal never touches the
block of an instance it sealed while attaching). Every failed-attach unwind releases its claim
only by ATTACHING to NONE, so it can never replace SEALED. An instance already ATTACHED when the
seal begins is drained by the seal; if the sink failed, the seal's last step makes it SEALED so the
owner refuses later boundaries and its detach is ESTOPPED.

On any refusal `instance` is 0 and nothing is installed. `ps3dk_prof_fifo_detach(instance)` on the
owner removes the observer and publishes the owner's final counters; it does not wait (the drain
thread writes the remaining events and the exact FINAL status). It is EBUSY from inside the
observer callback, ESTOPPED after the capture was sealed (the storage then stays allocated:
quarantined), EINVAL for another instance, EPERM from another thread.

The owner callback takes no lock, makes no SDK call, allocates nothing and never runs sink work.
It writes one fixed record into a single-producer ring (release publication of the write index)
or counts a drop when the ring is full; the drain thread consumes with acquire loads and advances
the read index only after the sink accepted the whole FIFO_EVENTS chunk. The FIFO drain step takes no lock and runs before
the SPU/GPU extension step of each drain. Independent progress also needs the owner to hold no
profiler mutex across a GCM reserve, and it does not: the GPU zone, mark and calibration paths call
the reserve with neither the domain snapshot mutex nor the extension mutex held (the snapshot mutex
covers only snapshot copies; the extension mutex covers attach, release, statistics and the
drainer's finalization, none of which reserves FIFO space), and the core's thread and descriptor
mutexes are never held across command emission. An application that held the extension mutex
itself across a wrap (it cannot through this API) would stop every later drain, FIFO included. The
host controls park the owner in phase 1, in phase 2, and inside a GPU zone's reserve (the owner
pinned in a GPU call) and require the open phase's boundaries to reach the sink before the wait is
released. A failed sink is terminal and nothing is retried. Recursive observation (an observer reached while it runs) is
suppressed in the GCM bridge and counted there.

Memory: one allocation at attach, ring records x 96 + 4096 bytes (102400 by default, 1 MiB default
ceiling), plus the static instance record; no thread-local storage is added and nothing is
allocated per event.

## Reader

`tools/ps3dk-prof/src/fifo.rs` decodes each chunk whole, validates it before committing it, and
refuses a chunk breaking any rule above as MALFORMED (rules FifoConfig, FifoEvents, FifoStatus).
Retained events are charged against a retention budget (`events`, `admission_ops`) before they
are stored; on exhaustion the inventory keeps what it had, names the refusal and keeps validating.
These budgets account for retention admission only: they do not bound the parser's total work or
its transient per-chunk validation allocation, since every later chunk is still decoded and
validated (and refused when malformed) after a retention refusal.

Pairing is exact or absent:

- A phase is COMPLETE when its BEGIN and END are both visible, have consecutive attempt sequences
  and valid clocks. Its PPU elapsed lies in [END.before - BEGIN.after, END.after - BEGIN.before].
- A phase whose BEGIN is the last attempt the capture knows of (no later event and no status
  showing a later attempt) is OPEN: its end is UNKNOWN, never zero.
- Any other missing endpoint, a sequence gap or an invalid clock makes the phase UNAVAILABLE.
- The outer wrap is COMPLETE only with all six events contiguous; OPEN when its last visible event
  is the last attempt; UNAVAILABLE otherwise.

`ps3dk-prof fifo [--json] capture.prof` lists every visible wrap with its phase states and bounds,
the entered and completed counts (visible and from the status), the owner token, and the loss and
refusal counters. Register samples, prepared and published bytes, RSX-reached markers and supply-gap
views are reported as not enabled in this capture, not as zero. The viewer's `FIFO wrap waits (PPU
clock)` page draws one bar per phase on the PPU timebase axis with open ends marked, and keeps the
RSX lane's execution status UNKNOWN.
