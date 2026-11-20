# Offline combined restart checkpoint

This is an internal host-side checkpoint format for the admitted PPU/WINDOWS
and SPU record families, with schema 2 adding AUTO and FIFO support.
`rolling_file::read_combined` joins this format to an
offline PS3DKROL v1/v2 physical snapshot. It does not enable an SDK rolling
sink, live capture, or the complete maximum-capture-bytes feature.
Manual GPU, waits, function samples, counters and dependency/proof families
remain outside this format. Ordinary-source AUTO report-ring CONFIG 2047/11
uses the version2 inner AUTO checkpoint described below; EXPORT remains refused.

The implementation is in `tools/ps3dk-prof/src/rolling_combined.rs` and its
private `rolling_combined_checkpoint.rs` codec. It uses the existing PPU and
SPU validators, plus the pure `rolling_combined_positions.rs` feasibility
check. The standalone PPU and SPU checkpoint formats retain their own
contracts; neither is relabeled as a combined checkpoint.

## Admission and claim limits

Admitted original flags are exactly 1, 3 or 7, optionally ORed with WINDOWS
and, for schema 2, FIFO64 or AUTO128:
EXT is required, PROFILE is optional, and ORDER requires PROFILE. The actual
original file header and version are validated and retained. No flag is
cleared to make a different reader accept the state. Other flags, unknown
chunks reject. Schema 1 admits SPU domains; schema 2 also preserves actual RSX
declarations for AUTO/FIFO. A mixed DOMAIN payload is validated as one original
chunk, not rewritten into separate synthetic declarations.

The earliest imported checkpoint contains bounded producer claims about
evicted history. CRCs, exact accounting, typed active witnesses and feasible
chunk positions do not authenticate that history or reconstruct omitted raw
events. Actual retained chunks pass the ordinary validators at their original
indices. A segment transition is established only when canonical advancement
has been performed and its resulting bytes compared with the next checkpoint.
The single-prefix APIs do not perform physical-chain comparison; the file
entry point performs it for every adjacent pair in the selected window.

The position proof establishes that there exists a disjoint allocation of
omitted PPU and SPU event chunks consistent with the exact metadata positions,
PPU incarnation lifetimes and SPU block endpoints. It does not establish the
actual evicted interleaving. No allocation chosen by the proof is serialized
or supplied to timing analysis.

The returned type is `RetainedCombined`, not a legacy full-capture `Analysis`.
PPU crossing spans remain left-censored. SPU results contain retained events
and retained endpoint causal bounds only. An evicted BEGIN may support a
retained END without becoming a retained endpoint or a completed retained
span. Source LOSS is independent of storage eviction.

SPU placement is explicitly **causal support only**. Raw-counter history,
modular tick lifting and RMAX1 have not been evaluated. The result carries
`PlacementBasis::CausalSupportOnly`, `CounterHistory::NotEvaluated` and an
explanatory note. Original PROFILE classification is computed against the
caller-selected review table; it cannot override source/order refusals or
promote these bounds into legacy tick/duration qualification.

## Offline API

* `checkpoint_combined(prefix, limits)` accepts the actual original file
  header followed by complete original chunks. It returns canonical checkpoint
  bytes. An early prefix may end before HEADER, CLOCKS or PROFILE has arrived.
* `advance_combined(checkpoint, payload, limits)` imports the checkpoint and
  accepts complete original chunks starting at its global next index. Empty
  advance is byte-identical to the admitted canonical input checkpoint.
* `read_combined_suffix(checkpoint, suffix, limits, options)` imports state,
  reads the actual suffix, finalizes both family validators, and returns
  retained PPU reporting and SPU causal-support reporting separately.

Checkpoint and advance refuse a torn chunk or TRAILER: neither establishes an
ordinary successor checkpoint. Suffix reading can return an incomplete torn
tail, with the next index stopping after the last complete chunk. A complete
TRAILER uses the ordinary final-entry checks, is charged against cumulative
metadata, and forbids following bytes. A torn or absent TRAILER cannot qualify
sources as complete.

The required canonical identity for complete, nonterminal original payload Q
is `checkpoint(P || Q) == advance(checkpoint(P), Q)`. It includes per-source
chunk counters, frame ranges, unresolved tokens and trailing SPU block
coalescing. Cuts and intervening PPU chunks do not create SPU support barriers.

## Schema 1 envelope

All integers are little-endian. There is no padding. The last four bytes are
CRC32 over every preceding checkpoint byte. All reserved fields are zero.

| Offset | Bytes | Field |
| --- | --- | --- |
| 0 | 8 | ASCII `PFRMIX01` |
| 8 | 2 | Combined schema: 1 |
| 10 | 2 | Outer header length: 80 |
| 12 | 4 | Section count: 4 |
| 16 | 16 | Actual original file header |
| 32 | 8 | N: next original chunk index |
| 40 | 8 | B: preceding original framed chunk bytes, excluding file header |
| 48 | 8 | Total section header/body bytes, excluding outer header and CRC |
| 56 | 4 | Required family mask: 3 (PPU and SPU) |
| 60 | 4 | Zero |
| 64 | 2 | PPU combined-context schema: 1 |
| 66 | 2 | SPU combined-context schema: 1 |
| 68 | 2 | Metadata schema: 1 |
| 70 | 2 | Token schema: 1 |
| 72 | 8 | Zero |

The combined schema rejects `N == u64::MAX` and byte frontiers for which
`16+B` overflows u64. The generic pure position checker independently permits
a maximum u64 frontier; its broader range does not alter this schema rule.

Four sections follow in exactly this order: metadata (kind 1), PPU cursors
(2), unresolved tokens (3), SPU journal (4). Each has a 16-byte envelope:
`kind:u16, schema:u16, count:u32, body_bytes:u32, zero:u32`. Every section
schema is 1. Unknown, duplicate, omitted, reordered or wrong-version sections
reject, as do trailing bytes or inexact lengths.

## Schema 2 envelope and family support

Schema 2 uses the same magic and original header/frontiers, an outer header of
96 bytes and six sections. The family mask is 3 plus 4 for AUTO and 8 for FIFO.
Offsets 64 through 74 contain six u16 schema values, all 1; bytes 76 through 95
are zero. Section kinds 1 through 4 retain their schema 1 meanings. Kind 5 is
AUTO and kind 6 is FIFO; an absent family has an empty section with count zero.
Schema 2 is required exactly when either original AUTO or FIFO capability is
present. Existing schema 1 encodings remain unchanged.

An AUTO section has count 1 and a 32-byte wrapper followed by its bounded
PFRAUTO1 state. Its inner version is1 for earlier CONFIG forms and2 for ordinary
ring CONFIG 2047/11; this does not change the PFRMIX01 schema2 wrapper. The ring
inner contract preserves two fixed-size cursors, original CONFIG216, OPS176,
COVERAGE528 and STATUS208, as specified in
[the AUTO checkpoint contract](ps3dk-prof-rolling-auto-checkpoint.md#ring-inner-schema-2).
Global byte conservation includes228 framed bytes for every ring STATUS,
including statuses no longer held as original payloads. The wrapper contains
the state byte length, the accepted OPS-list
count at the latest STATUS and zero reserved fields. A FIFO section has count 1 and a
64-byte wrapper, its bounded PFRFIFO1 state and 32 bytes per accounting-window
group. The wrappers retain original list/status positions and counts needed
to prove a possible interleaving; they never invent original chunk indices.

Common support metadata is stored once. Family CONFIG, all AUTO CLOCK/COVERAGE
records and the latest STATUS are preserved. Replaced STATUS records contribute
to exact historical byte/chunk counts, but do not consume metadata indefinitely.
Replacing metadata rebases its references without changing original positions.
The original global N/B conservation and disjoint position proof jointly cover
PPU records, SPU events, AUTO operations, FIFO phase lists, omitted statuses and
type 43 accounting rows. A closed window bounds its frames, not the later
emission time of an asynchronously drained accounting row.

The retained AUTO result uses actual retained operations and frame rows, plus
bounded build-frame and clock support from the prefix. Prefix execution and
FIFO phase boundaries remain explicit unavailability reasons. A saved build
population or clock anchor cannot become a retained operation. The reader
applies its existing qualified-map and verdict rules to the resulting evidence.

Combined admission charges family storage, other live state and conservative
analysis scratch against one temporary budget. It also charges metadata
replacement scans and family support scans before doing that work. Limits do
not reset at every retained segment, and a huge evicted count does not create
an allocation proportional to history. Exhaustion refuses the report rather
than pruning dependencies or relaxing final equations.

The [rolling CLI](ps3dk-prof-rolling-cli.md) exposes this typed retained report.
The C combined encoder and SDK runtime have their own admission gates; host
schema 2 reader support does not enable those producers.

## Shared metadata section

Each entry is `raw_bytes:u32` followed by one complete original framed chunk,
including its original type, payload length, index and CRC. Entries are
strictly increasing by original index, and every index is below N. No event
chunk or TRAILER can appear here.

This section owns each original non-event chunk exactly once: HEADER, CLOCKS,
PROFILE, PPU declarations and retirement, window metadata, and all admitted
SPU support. Metadata identity is in the original framing; there is no second
independent type/index claim.

If N is beyond an obligatory position, the corresponding anchor must exist:
HEADER at 0, CLOCKS at 1, and enabled PROFILE at 2. Missing anchors reject
before metadata parsers that depend on them are called. PROFILE elsewhere,
or without its flag, rejects. An early cut retains the remaining obligation
through original flags and N. PROFILE parsing uses the actual preceding
CLOCKS frequency.

Cumulative metadata accounting is exactly `16 + sum(4 + raw_bytes)`. The
initial 16 charges the original file header even though it lives in the outer
envelope. Terminal TRAILER bytes use the same charge during suffix reading.

## PPU cursor and token sections

Every declared incarnation has one row, including retired incarnations, in
strictly increasing nonzero incarnation order. Each row has the standalone
PPU cursor fields plus a u64 count of actual accepted RECORDS chunks:

```
row_bytes:u32, incarnation:u32
written:u64, gaps:u64, gap_count:u64, regerr:u64
next_seq:u64, max_span:u64, last_tb_or_zero:u64, next_frame:u64
current_frame_or_zero:u64
last_frame_id_or_zero:u64, last_frame_seq_or_zero:u64, last_frame_tb_or_zero:u64
state_flags:u32, stack_count:u32, frame_run_count:u32, zero:u32
record_chunks:u64
stack_count * 80-byte active stack witness
frame_run_count * (first_frame:u64, last_frame:u64)
```

The row length is exactly `128+80*stack_count+16*frame_run_count`. State flag
meanings, absent-field zero rules, active BEGIN/BEGIN_PARENT fields, topology,
frame context, affected-state checks, exact frame membership and final-entry
equations are shared with the standalone PPU adapter. The existing refusal of
exhausted frame-ID cursor states remains. No closed prefix span or raw
historical RECORDS payload is retained.

For one incarnation, let p be record_chunks and r be written+gap_count, the
physical row count. Require `p=0 iff r=0` and `p<=r<=32767*p`. Numeric producer
loss magnitude is not a physical row count. The per-incarnation chunk count
cannot be inferred uniquely from an old standalone PPU checkpoint; no guessed
upgrade is supported.

Tokens use exact 16-byte rows: `owner:u32, zero:u32, frame:u64`, in strictly
increasing (owner,frame) order. Owners must be declared. Tokens already resolved
by prefix frame membership are noncanonical here. Final token rules still use
exact prefix ranges and actual retained frames, with the existing genuine-loss
and incomplete-source conditions. Ranges are never expanded into frame IDs.

## SPU journal section

Entries preserve original SPU family order. A common 24-byte envelope is
`kind:u32, entry_bytes:u32, first_index:u64, last_index:u64`.

Kind 1 is a 32-byte support reference: `metadata_ordinal:u32, zero:u32`
follows. Its first and last indices are equal to the referenced original
metadata index. Every SPU support entry is referenced exactly once and in
order; common/PPU metadata cannot be referenced. No payload is duplicated in
the encoded SPU section.

Kind 2 is a support-free event block. `run_count:u32, zero:u32` follows the
envelope, then sorted nonzero lane runs. A run has a 64-byte fixed portion:

```
lane:u32, epoch:u32, first_seq:u64, events:u64, chunks:u64
known:u32, stack_count:u32, protocol_witness:6*u32
stack_count * (seq:u64, epoch:u32, raw:u32, desc:u32, depth:u32)
```

The existing standalone SPU typed validators check sequence tiling, epoch
transitions, known/unknown stack state, terminal protocol witnesses, active
BEGIN identity and exact source final totals. For each run,
`1<=chunks<=events<=131069*chunks`. Block endpoints are ordered, disjoint,
inside `[0,N)`, and cannot be metadata. A one-chunk block has equal endpoints;
a larger block has distinct endpoints. No SPU support lies inside a block.
Common/PPU metadata may lie inside it away from the required SPU endpoints.
Adjacent event blocks are noncanonical; only real SPU support splits blocks.

Within-block cross-lane event order is intentionally abstract. There are no
per-lane global chunk-position claims. Active SPU descriptors are image-local,
not PPU descriptor IDs. SPU_IMAGES, SPU_LANES, DOMAIN and EPOCH are support
barriers, so block import cannot use a later such declaration. Active witnesses
must have zero PPU frame owner and frame ID. The admitted cross-family owner
reference, SPU_LANES.frame_track_owner, is checked at that support's original
position against the PPU declarations already encountered.

## Conservation and position feasibility

Let M/L be the number/summed original framed lengths of shared metadata;
P/R the summed PPU chunk/physical-row counts; and S/E the summed SPU
chunk/event counts. Using checked wide arithmetic, require exactly:

```
N = M + P + S
B = L + 28*P + 32*R + 40*S + 8*E
SPU input bytes = SPU support framed bytes + 40*S + 8*E
SPU event total = E
```

The imported SPU journal's last index is also checked and must be below N.
It never determines the outer frontier by itself. PROFILE and other common
metadata are counted once. Neither PPU GAP loss magnitude nor SPU LOSS count
adds event bytes.

For each PPU incarnation derive the legal RECORDS interval from actual
metadata: `[THREADS index+1, RETIRED index)` or `[THREADS index+1,N)`. Slot reuse
creates another incarnation and another demand. Retired owners remain declared
for reference validation, but cannot receive new PPU records.

Reserve all metadata and required SPU block endpoints. A PPU incarnation
demands p remaining slots in its lifetime interval; a block demands its chunk
count minus reserved endpoints within its block interval. Rank-compress the
remaining positions and perform a batched earliest-deadline sweep. Exact full
occupancy is required: unfilled early slots cannot be skipped. Each batch
finishes a demand or reaches another release/deadline; huge multiplicities
do not add iterations.

With D incarnation/block demands and K metadata/endpoint anchors, temporary
state is O(D+K), and proof work is O((D+K) log(D+K)). Neither a per-event
expansion nor an RLE all-history position ledger is used. A fixed-identity
capture alternating PPU and SPU event chunks does not grow framing-history
state merely because it has more drain iterations.

## Import, advancement and failure handling

The codec first admits the encoded length, schemas, section/count shapes,
framing and mandatory bootstrap anchors. Metadata is replayed at its original
positions. Every SPU support call receives a borrowed view of the current
PPU descriptor/declaration registry, never a final dictionary from the future.

SPU blocks import after their last endpoint. Intervening PPU metadata can be
replayed first because admitted SPU_EVENTS references only existing SPU
lane/image/domain/epoch state. This commutation is specific to this family
subset and is not an extension rule for GPU/AUTO/FIFO.

Typed PPU cursors are installed after metadata replay. Exact conservation and
position feasibility must pass before a restored state is returned. Actual
suffix chunks then update the same validators and cumulative counters. Any
failure discards the private transaction; there is no published half-advanced
state or older-checkpoint fallback. Present retired/source final entries are
checked through the shared validators before checkpoint publication.

## Resource limits and memory interpretation

Defaults are:

| Resource | Limit |
| --- | --- |
| Encoded checkpoint, including all framing/CRC | 8 MiB |
| Cumulative metadata charge | 4 MiB |
| Original framed input per public operation | 64 MiB |
| PPU incarnations / descriptors | 4096 each |
| SPU domains / lanes / images | 1024 each |
| PPU frame ranges plus unresolved tokens | 65536 |
| PPU and SPU active-witness rows | 262144 combined |
| SPU compact event runs | 65536 |
| Actual PPU physical rows plus actual SPU events | 1,000,000 jointly |
| Weighted structural work | 2^24 units |
| Position temporary items | 262144 |
| Position work per proof / cumulative reservation | 2^20 / 2^24 |

These are independent ceilings, not promises that every input below each
individual numeric count will fit all other limits. Raw events have a
conservative structural-work charge in addition to their event-count charge.
The token/frame membership scans also require work admission; numeric prefix
event/frame counts are never expanded. Causal reporting has its own existing
work, group, output-row and finding limits and returns an atomic refused
causal view if those limits are exceeded.

Raw metadata byte copies are admitted by encoded/metadata byte limits before
copying. Source counts are admitted before typed declaration strings/rows are
allocated. Nested cursor/run counts and exact lengths are checked before
materializing their arrays. Position input vectors and proof queues share the
position-item limit: the orchestrator subtracts its own vectors' entries before
passing the remainder to the pure checker.

A public operation owns one budget across its import, suffix/advance and
publication stages. Each position invocation reserves its configured maximum
work from the cumulative proof budget; unused units are not refunded. The
physical-chain reader preserves that budget across all selected segments.
Calling independent public operations does not create such a chain budget.

`checkpoint_bytes` is an encoded-state limit, not a total process-memory cap.
During import, bounded original metadata and the SPU adapter's support copy
can coexist. The original encoded input is borrowed; typed cursors, active
witnesses and declaration dictionaries add their own bounded storage. During
encoding, section buffers coexist with the assembled output, and one SPU
block buffer may coexist with its journal. Successful section-buffer lengths
sum to at most the encoded body length; failed operations can reach an
individual buffer's cap before a later total-size check fails. Raw retained
events/spans are bounded by input/event/work limits, not checkpoint size.
Collection capacities and allocator overhead can exceed logical entry counts.
No exact resident-memory promise is made.

## Offline physical chain contract

`rolling_file::read_combined(bytes, store_limits, limits, options)` accepts a
coherent immutable snapshot supplied by the caller. It calls the physical
reader once and uses exactly that selected root chain. Physical recovery can
choose an older valid root when a newer physical commit is damaged; a semantic
failure in the selected chain never causes an additional fallback.

The first imported checkpoint must match the selected root's original first
chunk and evicted-payload byte count. These are checked after typed import and
the exact conservation/position proof. They bind bounded prefix claims to the
physical window; they do not authenticate evicted events or establish an
additional semantic session identity absent from the checkpoint schema.

All selected payloads update one live PPU/SPU state at their original indices.
At each segment boundary, canonical checkpoint bytes are encoded from that
state and compared byte for byte with the next stored checkpoint. Repaired
physical and checkpoint CRCs cannot substitute for equality. The reader does
not re-import the next checkpoint, discard retained events, or move the report
baseline. Thus complete work and crossing spans in every selected slot remain
in the same retained report.

A genuine original TRAILER is allowed only in the last segment. A later
segment rejects even if its payload is empty. No successor checkpoint is
fabricated after a TRAILER. Each segment's first/next original indices, the
root's final original index, and the exact final byte frontier are checked.
For v2 the accepted byte frontier also equals `bound_payload_bytes`. Shared
source-final validators run before any retained report is returned; canonical
equality at an intermediate boundary is not by itself a source-final verdict.

`CombinedFile.store_view` and `.retained` expose separate evidence. Physical
v1 state 0 is unspecified. V2 OPEN, SEALED and STOPPED_RESOURCE retain their
physical meanings and reason codes. SEALED does not manufacture missing
original TRAILER/source finals. OPEN may contain complete original semantic
evidence. STOPPED_RESOURCE does not turn semantic admission failure into a
successful partial report. A physically empty root has no semantic checkpoint
and returns `EmptyStore` even when SEALED.

The semantic join borrows checkpoint and payload slices. It adds at most 64
segment descriptors to the physical reader's at most 128 temporary descriptors;
it does not concatenate payloads. One canonical checkpoint buffer and bounded
encoder temporaries coexist with the live state. Encoded lengths and the
checked total payload size are admitted before import. Input bytes, actual
events, cumulative metadata, structural work and position reservations apply
to the entire selected chain, including comparisons and final reporting.
Empty segments still consume canonical-proof work. The default reservation
admits at most sixteen position proofs, including initial import: a legal
64-slot physical chain can require explicitly larger semantic work limits.

## Validation boundaries

`tests/rolling_combined.rs` uses independently written original captures and
the ordinary full reader before combined assertions. Coverage includes every
mixed cut, complete versus censored spans, full-source causal endpoints,
canonical repeated advancement, PROFILE placement/frequency, late-owner
forgery, repaired CRC/count/frontier/reference claims, incarnation lifetime
competition, slot reuse, constant-size alternating prefixes, terminal/torn
input and cumulative resource refusal.

The pure position checker has separate small-frontier exhaustive comparison,
huge-demand and mutation controls. Existing standalone PPU, advancement, SPU
structural and causal targets remain regression gates for shared helper
extraction. These controls establish the offline subset's tested behavior;
they do not establish runtime or full-family completion.

`tests/rolling_combined_file.rs` validates its original mixed source with the
ordinary reader and builds physical roots/slots independently. Named controls
cover three retained slots after rotation, every original first-cut boundary,
crossing spans and full-source causal bounds, canonical transition forgery,
prefix byte/index binding, empty/support-only segments, physical recovery,
terminal/source-final distinctions and cumulative chain budgets.

The explicit artifact test in `tests/rolling_combined_c.rs` uses the independent
C accepted-record model in `combined_fixture.c`. It compares thirteen C
checkpoint encodings with direct original-prefix encodings and reads the
actual C physical store into the retained report. This is stronger than an
opaque-checkpoint C packer but remains a fixture-level interoperability check,
not runtime integration or a proof for every producer history. Its ignored
status requires an explicit artifact invocation; an ordinary suite alone does
not establish that parity.

`run_combined_file_mutants.py` runs the existing named witnesses on isolated
copies. Its baseline must pass, and each mutant must compile and fail at its
expected witness. Compiler errors, missing tests, unrelated panics and process
failures are not sensitivity evidence. The runner does not rewrite production
files or weaken test assertions.

The private C schema2 encoder now accepts explicit FIFO/AUTO capabilities in
`pfm2_config`. It shares the PPU/SPU metadata budget and original chunk order;
FIFO STATUS and AUTO STATUS replace their own previous records independently.
AUTO state includes negotiated CONFIG, build populations, frame membership,
clock support, and both the current and STATUS-observed ring cursors. It stores
support for evicted operations without replaying those operations as retained
measurements. Admission and canonical encoding use one checked work budget.

The explicit C AUTO artifact tests compare every original-prefix checkpoint
with the Rust encoder. They cover CONFIG masks 7/15 through 2047/11, refused
CONFIG forms, build-versus-execution populations, abandoned builds, whole-CALL
coverage and tagged ranges. A separate fixture uses the real host AUTO runtime
to produce 560 ring records, including 520 in one frame, before invoking the C
checkpoint encoder. Repaired malformed-source controls require ordinary-reader
refusal first, then unchanged accepted C state and no successor checkpoint.
These are encoder interoperability tests: they do not connect the rolling
output consumer to live FIFO/AUTO producers or enable rolling SDK captures.

The physical-output fixture connects this encoder to the real private output
and store components. Its retained window crosses SPU events, FIFO phases and
ring generations while preserving an independently checked interior RSX span.
Capacity and write/sync failures must leave the incoming chunk unacknowledged.
Direct resource controls exercise exact checkpoint and metadata bounds, build
and membership pools, clock support, and the shared work allowance before CRC
scanning. These remain host fixture results, not target filesystem evidence.
