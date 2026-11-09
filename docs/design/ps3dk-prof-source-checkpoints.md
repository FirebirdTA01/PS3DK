# Source checkpoint evidence

This opt-in reader protocol validates recorded source completion separately from
whole-capture transport, raw validity, clock mapping and duration validity. It
uses file flag bit 3 and the membership and dependency evidence described in
[frame membership](ps3dk-prof-frame-membership.md). It does not implement the
producer, automatic frame completion, the completion viewer, or a hardware cost
measurement. Existing capture and lane verdicts retain their meaning.

All integers are little-endian. Standard chunk framing, size limit, CRC,
sequence and capability checks precede these parsers. Each complete metadata
batch validates before its facts publish. All reserved fields must be zero.

## ADMISSION, chunk 25

The prefix is 24 bytes: schema `u16=1` at 0, reserved `u16` at 2,
positive item count `u32` at 4, and the existing 16-byte source key at 8.
Exactly that many 32-byte items follow; the structural maximum is 32767.

| Item offset | Type | Meaning |
| --- | --- | --- |
| 0 | u32 | Origin frame owner incarnation |
| 4 | u32 | Epoch, zero for PPU; previously declared nonzero epoch for SPU/GPU |
| 8 | u64 | Origin frame id |
| 16 | u64 | Per-source admission id |
| 24 | u64 | Assigned BEGIN sequence |

The source, owner, frame and membership must precede admission. Admission after
that frame's CLOSE is malformed. Sources use separate `(kind,id,domain)` keys,
including when their numeric ids coincide. A source has increasing admission
ids across all frames and owners. Id zero and `u64::MAX` are invalid; BEGIN
sequence zero is valid but `u64::MAX` is invalid. A missing admission id makes
that source's inventory uncertain through capture end. Filling an earlier gap
or reusing a BEGIN identity is malformed. Membership uncertainty remains a
separate cause; it is not reported as missing admission metadata.

An admission assigns an identity, not completed work. Its raw events may be
recorded later. Already observed nonempty PPU/GPU BEGIN tokens must agree with
the admission. An absent token is unavailable attribution, not an invented
frame or a contradictory known token; it can support only noncertifying output.
A BEGIN identity occupied by a known non-BEGIN record is malformed, including
when that contradiction arrives after an earlier UNKNOWN terminal. SPU has no
recorded job/frame token in this schema and cannot certify
source completion.

## SOURCE_SEAL, chunk 26

One exactly 80-byte payload:

| Offset | Type | Meaning |
| --- | --- | --- |
| 0 | u16 | Schema 1 |
| 2 | u16 | Reserved |
| 4 | u32 | Frame owner |
| 8 | u64 | Frame id |
| 16 | 16 bytes | Source key |
| 32 | u64 | Exact preceding CLOSE chunk sequence |
| 40 | u64 | Source-wide cumulative admission attempts |
| 48 | u64 | Source-wide cumulative lost admissions |
| 56 | u64 | Producer's admitted count for this frame |
| 64 | u64 | Refused/suppressed attempts before this frame closed |
| 72 | u64 | Reserved |

Seals are unique per frame/source. The referenced CLOSE must exist and contain
this source. Unknown membership stays unknown. Attempts equal retained source
admissions plus lost admissions and cover the allocation frontier; neither
attempts nor loss may regress. Counters equal to `u64::MAX` are refused.
The seal advances the allocation frontier, including missing tail ids.

The frame count equals its retained admissions when that source ledger is known.
When uncertain, it must be at least the retained count and no more than source
attempts. This range does not prove the size of the missing population. The seal
acknowledges closure of producer admission; END records may still be pending.
Late rejected attempts are separate from the frozen pre-close refusal count.
This version adds no late-rejection opcode.

## SOURCE_CHECKPOINT, chunk 21

Prefix 64 bytes followed by exactly `count` 16-byte entries, maximum 65532:

| Offset | Type | Meaning |
| --- | --- | --- |
| 0 | u16 | Schema 1 |
| 2 | u8 | Outcome: 1 COMPLETE_RECORDED, 2 NO_DATA, 3 PARTIAL, 4 UNKNOWN |
| 3 | u8 | Reserved |
| 4 | u32 | Frame owner |
| 8 | u64 | Frame id |
| 16 | 16 bytes | Source key |
| 32 | u64 | Exact CLOSE chunk |
| 40 | u64 | Exact SOURCE_SEAL chunk |
| 48 | u64 | Reason mask |
| 56 | u32 | Resolved entry count |
| 60 | u32 | Reserved |

Each entry is an admission id `u64` and END sequence `u64`. Ids are strictly
sorted and unique. Frame, source, epoch and BEGIN sequence come from that exact
admission. Entries cannot borrow another frame's admission or a future record.
Both endpoints and the accepted pair must precede the terminal. PPU pairs need
an explicit matching BEGIN_PARENT token and must not be truncated or affected
at match. GPU endpoints need matching tokens and the admission's epoch.
SPU terminals are UNKNOWN with an empty resolved list.

Reverse coverage also checks accepted tokened BEGINs, including ones preceding
enrollment. A known sealed inventory cannot omit an operation entirely and
still certify completion. Missing admission metadata makes coverage uncertain;
it does not invent an admission. Tokenless BEGINs or GAP/LOSS after the source's
first accepted participation make attribution uncertain for that source. An
orphan END accepted by the legacy parser also latches this uncertainty after
participation. Legacy malformed-END rules still take precedence.

| Bit | Reason |
| --- | --- |
| 0 | Pre-close refusal |
| 1 | Accepted dependency loss |
| 2 | Unresolved admission, including one omitted from the resolved subset |
| 3 | Membership uncertainty |
| 4 | Admission inventory uncertainty |
| 5 | Attribution uncertainty |
| 6 | Epoch mismatch |
| 7 | Unsupported SPU job/frame evidence |
| 8 | Producer-reported failure |
| 9 | Producer-reported tracking capacity exhaustion |

Other bits are invalid. Bits 0 through 7 require corresponding evidence.
Bits 8 and 9 retain their producer-reported provenance. Reader policy refusal
is not a producer reason.

Public results keep the wire reason mask and independently derived predicates
in separate fields (`reasons` and `observed_reasons`). Reports show both sets
of causes: a producer cannot hide known inventory uncertainty by reporting only
another true cause such as pre-close refusal. No exact missing population is
inferred from an uncertain inventory.

COMPLETE_RECORDED is nonempty, resolves every admitted operation, and permits
no uncertainty, refusal or reason bits. NO_DATA has no admitted operation or
resolved entry and likewise permits no uncertainty or refusal. PARTIAL needs
a known omission (bits 0, 1, 8 or 9); UNKNOWN needs an unavailable premise
(bits 2 through 9). Both may retain a valid resolved subset. A terminal cannot
be replaced or upgraded. All outcomes require actual CLOSE and SEAL records;
missing prerequisites yield derived PENDING, never an empty successful proof.

## Publication, policy and reporting

Accepted raw batches are staged. The source observer validates before either
the source inventory or dependency index publishes the batch. Test-only commit
traces cover semantic errors after an otherwise valid first item. Historical
queries expose immutable accepted facts, not EOF diagnostics. Later loss or
policy refusal cannot rewrite an already accepted source terminal.

Source proof has independent PC defaults: 262144 entries, 64 MiB accounted
storage and 100 million work units. Dependency queries each retain their 4096
work limit. The source validator reserves up to five such query budgets per
admission cumulatively; unused reserved work is not refunded. This is policy
accounting, not elapsed CPU time. Stored admissions, seals, BEGIN inventory,
results and pending rows share the entry ceiling. Explicit vector capacities,
staging and replacement peaks are charged before allocation. Accounted bytes
are not process RSS or a target runtime footprint.

The first source-proof refusal is sticky and names its limit and chunk. Prior
terminals remain available; unvalidated later claims do not become results.
Missing dependency or membership evidence also makes source proof unavailable.
Pending-inventory construction includes accepted owner frames and BIND/ENROLL
intervals, including idle members without any admission or CLOSE. It has its
own charged final pass; if refused, the
CLI names that fact instead of showing a silently empty successful inventory.

The CLI adds `Source evidence:` only when this logical stream begins with a
type 21, 25 or 26 chunk. It lists source outcomes, evidence-backed reasons,
producer-reported reasons, and missing CLOSE/SEAL/checkpoint prerequisites.
Tokened BEGINs from a source missing from the recorded closed roster get an
explicit unrostered-source diagnostic with pending membership. Other sources'
independently completed results remain unchanged; no aggregate frame completion
is asserted. Membership-only and flag-clear reports receive no new section. No source proof
upgrades a legacy raw-validity badge or asserts valid timing or durable storage.

## Future producer cost and deployment gates

There is one admission item per successfully admitted tracked scope, not per
frame or per compiler function call. With A admissions, B nonempty admission
batches, F seals, T terminals and P resolved entries, added encoded bytes are:

`32*A + 44*B + 100*F + 84*T + 16*P`.

This includes chunk envelopes but excludes existing event, membership and
transport bytes. For complete scopes P=A. At 1000 scopes/frame, eight batches,
one seal and one terminal, this adds 48536 bytes/frame, or 2912160 bytes/s at
60 Hz (about 2.78 MiB/s). These are arithmetic examples, not benchmarks.

The future runtime must offer frame-level/named-scope filtering before tracked
admission and visibly identify that population. Omitting admission for a still
tracked tokened BEGIN is not a filter. Producer closure, loss latches, ownership,
queue publication and asynchronous GPU progress still require runtime controls.
No explicit application finalize call or per-frame GPU idle wait is required by
the intended contract. Console OFF-A/ON/OFF-B measurements must report filtered
and full costs, memory, stream rates and refusals beside baseline variation
before default enablement. Runtime, completion UI and hardware acceptance remain
separate gates.
