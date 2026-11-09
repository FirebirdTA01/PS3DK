# Frame membership and closure, schema 1

This slice validates frame participant inventories. It does not prove that any
participant finished its work, recorded all events, or supplied usable timing.
`InventoryState::Verified` means only that the recorded roster matches the
membership evidence and its owner boundaries are unambiguous. It must never be
displayed as COMPLETE_RECORDED, measured time, or proven NO_DATA. A missing
closure has no validated inventory; it is not an empty roster.

File flag bit 3 (0x8) enables FRAME_MEMBERSHIP (19) and FRAME_CLOSE (20).
FRAME_SOURCE_CHECKPOINT (21) is reserved and deliberately refused until its
dependency schema is implemented. This flag is independent of domain extensions;
foreign sources still require their usual declarations and capability flags.
PROFILE placement and SPU_ORDER prerequisites are unchanged. On the implementation
base, valid flag values are 0, 1, 3, 7 and their bit-3 variants 8, 9, 11, 15.
Other independently developed capabilities require a separate integration check.

The reader exposes this evidence through `Capture.checkpoints`, separate from
legacy capture verdicts and source raw validity. Without bit 3, kinds 19..21
remain unknown at the existing dispatch stage. Full-chunk length, CRC, sequence
and PROFILE-placement checks retain their existing precedence.

## Identities

All integers are little-endian. Chunk framing, CRC, sequence and maximum payload
remain unchanged. File identity comes from the containing HEADER; references
never resolve against another capture. Owner is a nonzero PPU incarnation.
Frame is a u64 id, including zero, with a preceding matching FRAME record.

A source key is 16 bytes: u8 kind, u8 reserved=0, u16 reserved=0,
u32 id, u32 domain, u32 reserved=0. Kinds: 1 PPU (id=incarnation,
domain=0), 2 SPU (id=lane id, domain=its declared SPU domain), 3 GPU
(id=domain, domain=id). Zero ids are refused. Source epochs are dependency
identities for the later source checkpoint, not new roster members. A source
that spans epochs stays one member; a replacement domain is a different member.

This file's declared metadata must already contain referenced owner and source.
No forward resolution, invented source or cross-file resolution is allowed.

## Membership record 19

One fixed-size operation per chunk, 48 payload bytes:

| Offset | Type | Meaning |
| --- | --- | --- |
| 0 | u16 | schema=1 |
| 2 | u8 | operation: 1 BIND, 2 UNBIND, 3 ENROLL |
| 3 | u8 | reserved=0 |
| 4 | u32 | owner incarnation |
| 8 | u64 | frame id |
| 16 | u64 | membership ordinal, first=1 |
| 24 | 16 bytes | source key |
| 40 | u64 | binding id (nonzero BIND/UNBIND, zero ENROLL) |

Ordinal counts every membership publication attempt, including one lost before
the sink accepts it. It is capture-wide, checked, never reused and never wraps.
Wire records have strictly increasing ordinals. A gap is lost membership, not
an empty inventory; duplicate/regressing ordinals are malformed. Binding ids
are capture-wide unique, strictly increasing on BIND, and never reused.

BIND starts a source/owner interval inclusive of the named frame. UNBIND names
that binding and its last included frame (inclusive); it cannot remove the
source from that frame's expected roster. A source rebound during the same
frame still occurs once in the set. A binding must be unbound before it is
replaced; independent explicit token admissions can still enroll other frames.
The writer must not publish UNBIND before all earlier admitted references have
recorded their original frame identity. An interval is not a completion result.

ENROLL adds an otherwise unbound source to that frame before its first successful
token admission. Repeated admissions from the same source/frame reuse the
existing enrollment; duplicate wire ENROLL is malformed. Admission counts and
event dependencies are not inferred from the number of ENROLL records.

The owner PPU source is implicitly expected from the referenced FRAME record.
Explicit self-enrollment is redundant and refused. A bound SPU source without
job tokens remains expected but cannot thereby become complete or known-empty.

## Close record 20

Fixed 48-byte prefix followed by exactly member_count source keys (16 bytes each):

| Offset | Type | Meaning |
| --- | --- | --- |
| 0 | u16 | schema=1 |
| 2 | u16 | flags: bit 0 membership_uncertain; no other bits |
| 4 | u32 | owner incarnation |
| 8 | u64 | closed frame id |
| 16 | u64 | successor FRAME sequence on this owner |
| 24 | u64 | membership_attempts at closure |
| 32 | u64 | membership_lost at closure |
| 40 | u32 | member_count |
| 44 | u32 | reserved=0 |

The preceding owner FRAME records must identify the closed frame and its direct
successor; skipped or loss-ambiguous boundaries cannot produce verified closure.
No wrap arithmetic or assumption that frame id equals event sequence is used.
The roster is strictly sorted by source key fields and contains no duplicates.
The reader derives the expected set from owner identity, binding intervals and
ENROLL; when membership is known, the sets must match exactly.

Close is unique per owner/frame and ordered within an owner. Once closed,
new admissions, enrollment or retroactive interval changes for that frame are
protocol errors. Already admitted operations may end later, under their original
frame, and cannot change the roster. Source terminal results are separate.

The membership stream and CLOSE records share a publication order. Every
membership attempt at or below the close frontier has either been accepted
before CLOSE or is included in membership_lost. A later record cannot fill an
old gap. At a close, attempts = observed distinct membership records + lost,
counting from capture start. Use checked arithmetic. The frontier never regresses.
This makes a missing final membership operation visible even with no later
membership record to reveal the ordinal gap. Duplicate unrelated records cannot
substitute for the missing identity.

## Sticky uncertainty and exhaustion

First missing membership, failed accounting or counter exhaustion latches
membership uncertainty for the rest of this capture. No recovery/snapshot opcode
in revision 1. Later closures stay UNKNOWN even if no new loss occurred.
Earlier accepted checkpoints keep their recorded evidence. Global scope is
deliberately conservative; this is membership uncertainty, not source-local
measurement loss and not a reason to rewrite legacy source validity.

The runtime holds this latch outside the membership queue. It is set before a
failed membership publication can be followed by successful closure. Ring loss
cannot erase the latch. A counter that can no longer represent attempts/loss
prevents further close publication; do not saturate into an apparently exact
equation. A failed CLOSE write leaves no accepted closure. Sink acceptance is
not power-loss durability.

If membership is uncertain, a parsed roster is explicitly unverified; it cannot
prove excluded sources absent. Unknown binding endpoints are not guessed. A
remaining UNBIND after its lost BIND is diagnostic uncertainty, not a reason to
invent a known binding. Structurally invalid ids, lengths, flags and reserved
bytes still refuse. Completeness is never obtained by suppressing a semantic
error that could otherwise be checked against known evidence.

## Reader bounds for the first slice

Reader work and retention are separate from runtime queue sizing. Initial
defaults: 262144 metadata records, 65536 distinct frame keys, 4096 members per
frame, 1048576 retained memberships, 64 MiB accounted checkpoint storage, and 100 million work units.
Existing header/domain/lane caps still apply. Reserve before allocation, checked
sizes before multiplication, and charge modeled searches and traversals before work. A limit reached yields a visible checkpoint refusal without invalidating
the legacy parse or silently calling an omitted member NO_DATA. Refusal is sticky
for later checkpoint metadata; already validated earlier results remain retained.
Only the existing 1 MiB payload cap is a structural wire cap; policy budgets are
not new reasons to call an otherwise valid file malformed.

These numerical defaults are starting ceilings to measure in the first reader
slice, not measured production memory bounds or performance claims.

## Reader policy and limits

The current implementation reserves storage allowances before retention. These
are accounting units, not measured allocator RSS or a target-memory footprint.
Work units are deterministic policy charges, not elapsed time or a benchmark.
The record allowance is 1024 + 32 * member_count bytes; temporary storage is
128 * scan + 32 * member_count bytes. The storage gate includes previously
retained allowances plus both new allowances. `scan` is one plus the counts of
bindings, enrollments, active bindings, retained closures, PPU lanes, owners
with a prior closure, and distinct referenced frame keys.

Before decoding a record, work charged is 128 + payload_bytes + 64 * scan +
64 * member_count. A frame lookup additionally charges 2 + bit_length of the
owner's recorded-frame count. Repeated small records therefore consume work
even if each record is individually small. The limits can refuse well-formed
metadata before the structural chunk-size limit is reached.

`Inventory.refusal` is sticky. After refusal, later checkpoint semantics are
not validated or retained; callers must show that limitation. The outer reader
continues to validate chunk framing, CRC and sequence. Kind 21 remains refused,
and checkpoint payload lengths remain checked. Previously accepted closures
are retained, but a malformed suffix still makes the file-level parse fail;
retention is observable through an earlier accepted prefix, not a successful
whole-file result for corrupted bytes.

Owner GAP handling is conservative: because the existing lane model retains a
total GAP count, any GAP already parsed on that owner makes a new closure UNKNOWN.
Nonconsecutive owner frame IDs also make it UNKNOWN. A later GAP does not rewrite
an earlier accepted closure. This is separate from capture-wide membership loss.

A retained UNBIND for a lost BIND establishes that its binding id was already
allocated. Future BIND ids must exceed that id; this does not reconstruct the
lost interval or restore verified membership.

## Producer obligations and deferred work

### Accepted dependency identities

Files with bit 3 also build a capture-owned dependency index. This adds no wire
record: type 21 remains refused. The index is a prerequisite for a later
dependency validator, not an implementation of terminal source checkpoints.

`Capture::dependency_record` identifies an accepted record by source kind, id,
domain and sequence. Optional epoch and record-kind constraints are additional
equalities. Sequence is never substituted with a count, timestamp or another
source's sequence. PPU records have no epoch. GPU token fields are retained as
written; NONE and differing endpoint tokens are not repaired. SPU records have
no job/frame token. A GAP or LOSS occupies one sequence interval and returns
`Lost`, never an event, including when an epoch was requested for that interval.

Every immutable record fact carries its chunk sequence and item index.
`before_chunk = C` accepts facts only from chunks strictly earlier than C.
`Capture::dependency_pair` additionally requires the accepted BEGIN, PPU
BEGIN_PARENT where applicable, END and matching fact to precede that boundary.
An unpaired END does not create a matching fact. PPU truncation and affected
state at matching are explicit pair diagnostics. Later loss, epoch changes or
source errors cannot rewrite these immutable facts. Historical diagnostics not
represented by these facts remain unavailable; callers must not project the
source's end-of-file state into a historical query.

The index publishes a batch only after its complete chunk passes structural and
record validation. A malformed suffix still refuses the whole file. Earlier
facts are available by reading the earlier accepted prefix, not by treating a
malformed full file as usable. Source-global validity and legacy frame metrics
are unchanged; an indexed event or pair does not establish frame completion,
timing validity or absence of unrecorded work.

PC-side defaults are 262144 retained entries (event/loss entries plus pair
facts), 64 MiB of storage accounting and 100000000 ingestion work units.
`reader::read_with_evidence` accepts independent membership and dependency
limits. Files without bit 3 construct neither index. Staging and retained
vector capacity are accounted using host `size_of`. Source containers and
BEGIN-keyed pairs use sorted vectors so there are no opaque tree-node capacity
allowances. Vector growth reserves a possible replacement while the old
allocation and staging remain live. Excess reported vector capacity is refused
before publication. These are
accounting limits, not measured RSS or PS3 memory requirements.

Ingestion charges END-count scans before invoking them, new entries, source and
pair searches, and the copying and shifting of retained vectors. A sticky
refusal skips subsequent index-only scans. Arithmetic overflow refuses admission.
The first refusal is sticky and names its chunk; prior retained evidence remains
queryable. Unretained evidence after refusal returns `Unavailable(IndexRefused)`
only when the refusal precedes the requested query boundary; a later refusal
cannot rewrite an earlier missing-identity or missing-pair answer.
`IndexStatus.records` counts retained event/loss entries, so a large lost interval
counts once and does not report a number of observed events.

`QueryBudget` defaults to 4096 work units per call and meters source lookup,
interval search, pair lookup and endpoint checks. Exhaustion returns
`UNAVAILABLE (query work limit)` without changing the index or another query's
budget. Missing data is never represented by an empty successful answer or
`NO_DATA`. Exact policy charges are defined by `dependencies.rs`, not a CPU-time
estimate.

The CLI report names `Dependency evidence: INCOMPLETE` when ingestion refuses,
including the reason, stopped chunk, retained counts and configured ceilings.
Overview displays the same per-capture warning. Replacing or refreshing a
document replaces the warning with the new capture's state. A successfully
indexed capture is labelled `INDEXED`, with no completion claim.

This reader checks recorded consistency. It cannot detect a successful bind or
admission omitted everywhere by the producer. Before runtime deployment, controls
must prove that every successful binding and admission publishes its membership
record or latches uncertainty outside the lossy queue. Membership and closure
must share publication order. The runtime requirements above are obligations,
not evidence that a runtime implementation has satisfied them.

Source completion still needs accepted event identities, original frame
ownership and dependency coverage. Source epochs belong to that later proof.
Runtime publication, the source checkpoint payload, completion UI, and hardware
acceptance remain separate work; this slice adds no automatic-completion claim.
