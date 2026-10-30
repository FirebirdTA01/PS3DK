# Binary arc companion format v1

This separate file kind records physical PPU calls. The v1 timing format is unchanged.

## File kind and common primitives

Use the existing 16-byte envelope: `PS3DKPRF`, u16 major=1, u16 minor=0,
u32 kind=1. All fields are little-endian without padding. The word at offset12
is still required to be zero by the frozen `.prof` reader. Only the new `.arcs`
reader accepts exactly1; neither reader accepts other bits/kinds. Thus an old
reader refuses this artifact rather than treating it as timing data.

The `.arcs` reader uses the same bounded LE cursor and chunk framing:
`u32 type, u32 len, u64 chunk_seq, payload[len], u32 crc`. CRC is the existing
IEEE CRC32 over chunk header and payload. Max payload1048576; sequence starts0
and increments exactly1, without wrap. UTF-8 strings use u16 byte length<=512,
no NUL; build identity is a raw length-prefixed byte string as in `.prof`.

Bad header/kind/major, unknown chunk, nonzero reserved bits, full bad CRC,
out-of-order sequence, impossible length or bytes after TRAILER => MALFORMED.
Truncated header => MALFORMED; valid envelope followed by a torn chunk or missing
TRAILER => INCOMPLETE. Higher minor gives a warning but never relaxes unknown
type/field rules, matching the existing version policy.

The timing reader entry point retains its existing behavior and all193 reader
tests. Shared byte-envelope helpers may be extracted only with unchanged timing
fixtures/verdicts; semantic validation remains specific to the file kind.

## Chunk payloads

| Type | Payload |
|---|---|
| 1 HEADER | Six strings: app, build_id(raw), sdk_version, compiler_identity, flags, workload_tag; u8 abi (1 ILP32,2 LP64); u8 mode (1 pg-only,2 dual-hooks); u16 snapshot_flags (bit0 FINAL, no other bits); u64 session_id(nonzero), u64 snapshot_id(nonzero), u64 timing_prefix_bytes, u64 timing_chunk_seq; u32 timing_prefix_crc32; u32 max_threads(1..1024); u32 max_arcs_per_thread(1..65536). |
| 2 CLOCKS | Exactly the `.prof` payload: count1, domain1(PPU_TB), reserved0, nonzero u64 freq_hz, u64 tb_at_init. |
| 4 THREADS | Same payload/identity rules as `.prof`: count>=1, then incarnation!=0, slot<max_threads, u64 OS thread ID, name. |
| 8 ARCS | u32 incarnation; u32 n>=1; n entries of `{u64 caller_return_pc, u64 callee_hook_pc, u64 count, u32 flags, u32 reserved}`. Length exactly8+32n. |
| 7 RETIRED | u32 count>=1; count final entries described below, each state RETIRED. |
| 6 TRAILER | u32 end_flags; u64 sink_error_count; i32 first_sink_error; u32 n; n final entries. |

HEADER is chunk0, CLOCKS chunk1, exactly once each. Then THREADS/ARCS/RETIRED,
then at most one final TRAILER. Timing DESCRIPTORS/RECORDS are not allowed here.
Only declared, nonretired incarnations may occur in ARCS. Slot ownership and
retirement/reuse follow the existing timing rules. Every declared incarnation has
one final entry, in RETIRED or TRAILER, never both. Missing finals are INCOMPLETE.

## Arc identity and counters

PCs are explicit u64 wire values in both ABIs, never native `uintptr_t` layout.
`callee_hook_pc` is the code return PC after the injected `bl _mcount`, not an
OPD descriptor or an already-symbolized function entry. `caller_return_pc` is the
profiled function's caller return PC recovered according to the measured PPU hook ABI.
Both known PCs are nonzero and4-byte aligned. Flag bit0 means unknown caller;
then and only then caller_return_pc=0. No other flags, reserved=0. Callee is never
unknown. These records contain physical mcount observations, not cyg inline spans.

Keys `(incarnation, caller_return_pc, callee_hook_pc, caller-unknown)` are unique
within one artifact, even across ARCS chunks. Count is1..UINT64_MAX. At most
max_arcs_per_thread distinct keys per incarnation. Duplicate, zero count, bad PC,
capacity violation, declaration/order violation => MALFORMED. The host sums with
u128 checked arithmetic; it never wraps a sum back into u64.

Each artifact is a cumulative snapshot since session start, not a delta to add
to a prior snapshot. Host live views replace a prior snapshot with a strictly
newer snapshot_id; they do not sum snapshots. Older/duplicate IDs are refused.
The registered-thread incarnation identifies reuse; OS IDs alone are not keys.

Final entry is40 bytes: `u32 incarnation, u8 state, u8 limit_flags, u16 reserved,
u64 attempted, u64 recorded, u64 refused, u64 lost`.

- State1 RETIRED,2 ACKED,3 NOT_QUIESCED, matching existing lifecycle meanings.
- Known states1/2: flags0 or bit0 COUNTER_LIMIT. All counters are exact u64;
  `sum(encoded arc counts)=recorded` and `attempted=recorded+refused+lost`, checked
  in u128. A mathematical sum outside u64 is MALFORMED for a known final entry.
- State3: flags exactly bit1 COUNTS_UNKNOWN; all four counters UINT64_MAX.
  No exact-counter equality is asserted. Encoded arc rows remain observations,
  not a certified total. In known states UINT64_MAX is an exact value, not a
  sentinel; state makes the distinction explicit.
- Any other flags/state, reserved!=0, duplicate/missing declaration => MALFORMED.
- Refused counts were rejected before being represented; lost counts were
  accepted but cannot be represented in the retained snapshot. Either nonzero
  value disqualifies exact totals for that incarnation. Counter limits and
  NOT_QUIESCED also disqualify it. Never show a partial total as a clean zero.

TRAILER end_flags use bit0 NOT_QUIESCED, bit1 COUNTER_LIMIT, bit2 SINK_ERROR,
no others. Required bits must agree with final states/limit flags; sink count0
requires first_sink_error0 and no bit2; nonzero count requires bit2 and a nonzero
first error. Any sink error makes the whole artifact INCOMPLETE. Any lane with
refused/lost counts, COUNTER_LIMIT or state3 makes it INCOMPLETE, including when
all its retained counters still balance. Strict default combined reports
and gmon export require COMPLETE and qualified selected incarnations.

## Binding the timing companion and live prefixes

For pg-only mode, timing_prefix_bytes=0, timing_chunk_seq=UINT64_MAX,
timing_prefix_crc32=0; standalone arc inspection is valid. It cannot produce a
combined timing report without a timing artifact.

Dual mode binds a specific `.prof` prefix: nonzero byte length ending at a full
chunk boundary, the last chunk's exact sequence, and IEEE CRC32 of those exact
prefix bytes including the timing file envelope. This uses the existing checksum
primitive; it is a corruption/identity cross-check, not authentication. HEADER
identity strings, ABI, session_id, CLOCKS frequency/init time and thread identities
must also agree. Missing/mismatched identity, frontier or checksum refuses the
combined report, without rewriting either artifact's standalone verdict.

For FINAL, the prefix is the entire timing artifact including its TRAILER; both
must be COMPLETE for exact combined output. For a progress snapshot, the prefix
may lack a timing trailer; live display must say provisional/incomplete. It cannot
be exported as a certified final report or gmon file. Snapshot publication needs
producer acknowledgements that capture an arc-counter image and timing frontier
without racing writes; unacknowledged threads use state3. That runtime handshake
is implemented/tested in the later lifecycle/transport tasks, not assumed by a
host parser of encoded test bytes.

For progress matching, parse/analyze ONLY the bound prefix. Later appended bytes,
registrations, retirements or even an invalid later tail cannot change this older
snapshot's result. Compare the complete set of thread incarnations declared in
that prefix with the companion's declared set, including retired incarnations;
require exact slot/OS-ID/name matches. The prefix length must equal a complete
chunk boundary, not just have a matching checksum. FINAL still requires exact
whole-file length and rejects any trailing bytes.

No timing format field is repurposed and no invented timing event represents an
entry-only arc. A snapshot does not require exact equality between mcount calls
and cyg spans: inlining/tail calls and their different domains are explicit.

## Binding automatic timings to functions without changing .prof

Automatic cyg descriptors use scope1, file empty, line0, logical_tag exactly
`ps3prof:auto:cyg:v1`, and name `ppu-fn-opd:` followed by exactly16 lowercase
hex digits of the nonzero function DESCRIPTOR address. Descriptor IDs retain
the existing FNV rule unchanged. Other descriptors are manual annotations and
cannot silently supply automatic function timing. A malformed descriptor in the
reserved automatic namespace refuses projection; an empty/unknown mapping is
never converted into a zero duration.

The verified ELF map resolves that descriptor at an8-byte entry offset relative
to `.opd` start, bounds-checks both4-byte words, reads the first word as big-endian
u32 code EA, and requires a4-byte-aligned address in an executable section.
Absolute8-byte descriptor alignment is NOT required: compact entries may start
at an address congruent4 mod8. The map is tied to the capture's ABI/build identity.
The host validates the GNU build ID of an ELF64 BE executable before mapping.
Both PPU data models have e_flags=0: the ABI is bound through the capture/build
identity, not inferred from ELF class or OPD width. Full DWARF display is a later increment.

Project the timing forest onto automatic descriptors: manual annotation nodes do
not become functions and do not consume a function's exclusive time. The direct
automatic children of a function span are its nearest descendant automatic spans,
even through manual-zone nodes. Exclusive function time is inclusive duration
minus those children's inclusive durations, with checked arithmetic. Censored,
affected, clock-anomalous or unresolved automatic spans refuse exact projection.
Do not reuse the manual-zone exclusive value if manual children were subtracted.

Inline logical cyg spans remain functions in this projection. Their physical
mcount arc count may be zero; no arc is synthesized to make them agree. Sidecars
and host reports explicitly distinguish logical timing spans from physical calls.
Gprof's propagated graph timings are a compatibility view, not exact per-edge
inclusive/exclusive times; the original timing capture stays authoritative.

## Host API and JSON export

New `arcs.rs`: ArcCapture, ArcHeader, ArcEntry, ArcFinalEntry, ArcVerdict,
ArcError/ArcRule; `read_arcs(&[u8]) -> Result<ArcCapture, ArcError>`.
New `companions.rs`: `match_companions(&ArcCapture, prof_bytes: &[u8])`
returns a verified pair or an explicit refusal. It invokes the existing timing
reader/analysis and validates the prefix identity/CRC/frontier above. A typed
FunctionProjection performs the descriptor/ELF mapping and automatic-only forest
projection specified above; the gmon exporter accepts only this verified input,
not arbitrary manual descriptor labels or already-rounded displayed values.
Only the host offers `export_arcs_json(&ArcCapture) -> String`; counts are decimal
strings and PCs fixed-width hex strings so JSON consumers cannot round u64 values.

Extraction of common cursor/framing routines will be a separate small diff within
this format, covered by the unchanged existing reader tests and malformed new-kind tests.
Do not expose constructors that let callers fabricate a verified pair.

## Derived gmon export policy

Export one file per selected thread incarnation; cross-thread elapsed sums are
not silently presented as one wall-clock profile. Preserve the exact `.prof` and
`.arcs` originals. Mode, chosen thread, identity, clock and quantization are in a
sidecar and our host report; stock gprof prints its own sampling-oriented wording.

- Default rate1000 units/second; explicit rate option1..1000000.
- For each canonical function, convert exclusive elapsed ticks to units by
  round-to-nearest, ties-up, using checked u128 arithmetic. Error <=0.5/rate
  seconds per function; disclose that bound and that elapsed timing includes
  instrumentation/preemption and compiler hook-boundary effects.
- Function units must fit u16(<=65535), otherwise refuse with an explicit rate/
  representability error. No saturation or hidden rate change.
- Emit disjoint 4-byte address-range histograms at canonical function entries,
  one bin each, same rate. Canonical aliases sharing an entry merge before bounds
  checks. Multi-range parsing is tested against gprof as a byte-format control.
- Both target ABIs use big-endian gmon version1,64-bit VMAs and32-bit tagged arc
  counts. Normalize raw return/hook PCs through the verified ELF map; do not feed
  descriptors as code PCs. Unknown/unmapped callee refuses the export; unknown
  caller is retained in exact data and refuses exact-call-graph export.
- Sum counts for each canonical emitted arc first, then require <=UINT32_MAX.
  Repeated smaller records are not a workaround: shipped gprof wraps 2x2^31 to0.
  This gets an explicit RED-before/GREEN refusal test.
- Actual symbol/file-line gprof compatibility remains blocked by t_bee3e3dd.
  Host writer tests and framing tests proceed now; no full compatibility PASS
  until the toolchain owner's fix passes the preserved Task1 fixtures.

## Serializer and host commands

`ps3dk-prof check-arcs capture.arcs` returns 0 for COMPLETE, 1 for INCOMPLETE,
2 for MALFORMED, and 3 for usage/I/O failure. `arcs-json` uses the same exit
statuses and exports identities, exact counters, PCs and qualification. JSON
counts are decimal strings; raw build identity is hexadecimal.

The internal `prof_arcs_writer.h` serializer accepts a stable, caller-owned
snapshot and a write-all callback. It validates input before emitting any byte,
uses fixed stack memory, emits one bounded ARCS chunk per row, and stops at the
first callback error. It does not allocate, format JSON, register threads or
provide a publication handshake. A failed write leaves incomplete evidence;
callers must not retry a partially written chunk in place. Compile the serializer
without instrumentation, like the rest of the profiler runtime.

`arcs-writer-test.sh OUTPUT_DIR` compiles the actual C serializer in plain and
ASan/UBSan modes, checks independent encoded bytes, and validates them through
the candidate reader supplied in `READER`. The existing manual-runtime driver
and reader fixtures remain unchanged.

The gmon API returns bytes and a provenance sidecar without replacing or modifying
input captures. It requires final verified companions and an identity-checked ELF.
Automatic timing, not sampled CPU time, is the authority. Manual-only annotations
cannot masquerade as automatic function timings. PC sampling belongs to a separate
later file kind and capability path.
