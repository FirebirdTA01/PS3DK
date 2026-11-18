# Private rolling capture retention inspection

`sdk/libps3dkprof/src/rolling/roll_retention_inspect.{h,c}` supplies bounded,
read-only physical identity and terminal framing evidence for a closed rolling
file. It is an internal foundation, not an enabled SDK rolling option or the
mixed-format retention implementation.

Cold success does **not** authorize enrolling a newly completed capture. That
requires the runtime's trusted successful semantic seal, close and worker join,
with final result OK. OPEN, resource-stopped, incomplete, close-failed and
join-failed captures do not qualify. The inspector does not validate source
counter equations, checkpoint transitions, extension finals or clocks. CRC32
detects accidental corruption; it neither authenticates nor signs identity.

## Private API and bounded storage

`pfi_measure` reads the configuration page, validates its checksum and layout
through `pfr_plan`, and checks the caller's file, slot and checkpoint ceilings
before returning storage requirements. The caller supplies at least4096 work
bytes. The function does not select a root or claim completion.

`pfi_inspect` remeasures rather than trusting a saved plan. Its caller supplies
aligned store storage, one full-slot scratch buffer, and at least4096 work
bytes. Buffers must be mutually disjoint and must not overlap inputs/output.
The helper performs no allocation, file open/close, write, resize or sync.
There is no automatic allocation fallback for an older, larger-slot capture:
insufficient limits or storage return SPACE. The eventual retention caller
must preserve that file and report its retention error.

The caller limits are maximum physical file bytes, physical slot bytes,
encoded checkpoint bytes and counted metadata bytes. Metadata is charged as
`16 + sum(4 + framed_chunk_length)` separately for each checkpoint. All parsing
walks bytes in the bounded file or checkpoint; it never expands evicted event
counts. Whole-file hashing uses4096-byte pieces. The existing physical reader
also uses fixed root-candidate stack storage, which is separate from the
caller buffers and must be included in the target stack review.

Positive short reads are retried. Zero, negative and overlarge read results
fail. The adapter owns interrupted-call handling. The caller must provide a
closed, coherent immutable file and serialize access; size rechecks and CRCs
do not establish a snapshot against concurrent same-size edits.

Outputs are published only on success and remain unchanged on every error.
The returned evidence tag is exactly `PFI_IDENTITY_FRAMING_FINAL`; there is no
semantic-validity or completed-capture boolean. Physical session16 is returned
as opaque data. No invented mapping relates it to the original HEADER's u64
session identifier.

## What the inspector checks

* Physical format2 configuration, roots and selected slots through `pfr_open`
  and `pfr_get_segment`. It requires a nonempty selected SEALED chain with no
  resource reason. It exposes the physical recovered flag. It does not retry
  an older root after a checkpoint or terminal inspection failure.
* PFRCKP01 schema1 or PFRMIX01 schema1/schema2 envelopes, inner CRC, fixed fields,
  section order/schema/lengths, admitted original flags, and each checkpoint's
  original chunk/byte frontier against its selected physical segment. Unknown
  versions and source capabilities remain explicitly unsupported. Schema2
  requires the exact 96-byte header, six sections, capability/family mask and
  AUTO/FIFO wrapper lengths and presence rules. Wrapper contents remain opaque;
  cold inspection is not semantic validation of family checkpoints.
* Counted metadata framing, CRCs, increasing original positions below the
  checkpoint frontier, and required original HEADER0/CLOCKS1/PROFILE2 anchors.
  Event and TRAILER chunks cannot be presented as metadata. Non-metadata cursor,
  token, SPU journal and AUTO/FIFO state bodies remain opaque to this helper.
* The actual HEADER payload's six length-prefixed strings and exact fixed
  tail. String0 must equal the caller's app bytes exactly; the original u64
  session must equal the caller's expected session. Original16-byte file
  headers agree across selected checkpoints. For an initial zero checkpoint,
  the retained payload supplies the actual HEADER at original chunk0.
* Payload framing and bootstrap positions, and a genuine clean TRAILER as
  the final chunk in the final selected segment. Even an empty later segment
  invalidates terminal placement. The TRAILER has exact size `40 + 56*n`,
  canonical reserved/state/limit framing and no explicit incomplete/error
  status. Its counters are not reconciled against source history here.
* Final physical chunk/byte frontiers and a CRC over **every physical byte**,
  including both roots, spare/unreferenced slots and padding. The digest is
  suitable for comparing a later cold inspection with an enrolled index entry,
  subject to the stated CRC and file-ownership limits.

The earliest checkpoint remains a producer claim. In particular, neither a
repaired-checksum forged prefix nor a physically SEALED container is evidence
that an application completed semantically. Runtime completion authority must
remain separate when connecting this helper to the retention index.

## Verification and sensitivity

From the repository root, with a fresh caller-selected external output path:

```sh
python3 tests/host/ps3dk-prof-rolling/run_retention_inspect.py \
  --cc cc --build-dir /path/to/inspector
python3 tests/host/ps3dk-prof-rolling/run_retention_inspect.py \
  --cc 'cc -fsanitize=address,undefined -fno-sanitize-recover=undefined -fno-omit-frame-pointer -no-pie' \
  --build-dir /path/to/inspector-sanitized
python3 tests/host/ps3dk-prof-rolling/run_retention_inspect_mutants.py \
  --cc cc --build-dir /path/to/inspector-mutants
```

The33 controls use independently constructed Python checkpoints/containers and
a C memory-IO adapter. Each fixture is checked independently with the physical
reader before inspector assertions. Positive PPU and combined fixtures cover
both evicted HEADER and retained chunk0. They deliberately contain no event
rows: these are ownership/framing tests, not semantic-family qualification.
Four full original source companions permit separate ordinary-reader checks.

Sensitivity requires the unchanged33-control baseline, then14 independently
compiled changes with named existing witnesses: exact app/session, SEALED
state, actual TRAILER, terminal-before-empty placement, checkpoint inner CRC,
chunk/byte frontiers, full-file digest, file/slot/checkpoint/metadata ceilings,
and accidental sync during cold inspection. A compilation failure, timeout,
crash, unrelated refusal or malformed result is a harness failure, never a
killed mutation. The digest and IO checks require the specific wrong digest or
nonzero mutation-call count while other successful-result fields still match.
The writes-disabled mutation adds an accidental sync; removal of callback
stripping alone is not claimed observable with today's read-only pfr path.

The mutation runner copies a bounded source inventory into a fresh external
directory, records hashes and logs, and verifies checkout bytes remain
unchanged. `--list` only validates mutation anchors and prints witnesses; it
does not compile or execute the controls.

Compile the helper and physical store as C11 with warnings treated as errors
for default PPU ILP32 and `-mlp64`. Before runtime wiring, inspect compiler
stack-usage output for both ABIs and the complete caller/callee/PAL chain.
Individual frame measurements are not a complete maximum stack bound. Host
sanitizers and cross compilation do not measure console filesystem behavior,
worker-stack headroom or power-loss durability.

## Remaining integration

The selected retention policy is newest N completed captures per exact app
across `.prof` and `.profr`, sharing one completion order and lock. The shared
retention implementation tags each version2 record's format and migrates
version1 under that lock without changing existing order. Duplicate app/session
across formats refuses. Its rolling branch measures before allocating full-slot
scratch and requires successful inspection, current-file sync and close before
publishing the index. An old-file inspection failure can leave the current
completion enrolled and count above N; a later completion retries that eviction.
This index implementation and the dependency wiring below do not enable a
rolling producer. Actual rolling lifecycle integration must supply explicit
budgets and runtime result-OK enrollment after semantic seal, close and join.

## Explicit archive dependency amendment

The shared mixed-format retention implementation calls the private inspector
from the PAL. Its dependency chain is explicit at every optimization level:
`pal_lv2`/`pal_host` → `roll_retention_inspect` and `roll_file` → `roll_store`.
No weak replacement or optimizer removal is used to satisfy linkage.

The manual and automatic lifecycle archives therefore add exactly
`roll_file.o`, `roll_retention_inspect.o`, and `roll_store.o` to their common
members. The extension and PROFILE extension archives add the same three;
WAIT, FUNC, COUNTERS, FIFO and AUTO variants inherit them from the common or
extension member list. The separate extension-live archive keeps its three
existing live members. SDK recipes provide the same sources for ILP32 and
LP64, including each explicit extension/PROFILE object directory. Public
headers, public options and rolling output remain disabled/unchanged by this
dependency amendment. Semantic encoders are not added to the archives here.

Exact member contracts in the release-tree gate, its independent fixture,
SDK install test, PPU archive test and extension checker include these three
names. PROFILE archive digests consequently include their actual member bytes;
an old digest is not relabeled as the new archive identity. Host and sample
harnesses that compile a PAL or include the retention implementation list all
three C files explicitly, including the standalone retention driver.

The extension import contract distinguishes three source-traced additions to
its historical baseline:

| Source | Additional external libc symbols |
| --- | --- |
| Existing completed-file retention, previously absent from that historical contract | `calloc`, `fstat`, `stat`, `rename`, `unlink`, `memmove`, `strrchr` |
| Bounded rolling inspection and exact basename comparison | `malloc`, `strcmp` |
| Existing early-exit support (#136), omitted by its historical contract | `at_quick_exit`, `pthread_mutex_trylock` |

Commit `cc0283378` added `prof_core.c:init_gate`'s registration of
`p_quick_exit_drain` through `at_quick_exit` and `pal_lv2.c:pal_mutex_trylock`'s
call to `pthread_mutex_trylock` for the bounded quick-exit lock path. The archive
contract at that same commit still omitted those two imports. They apply to
both data models and are separate from the rolling dependencies. The real
target archive gate exposed the stale expectation; the amendment follows
these call sites, without admitting other observed names.

The existing `fsync`, `read`, `lseek`, allocation/free and string/memory imports
remain where already required. `sysLv2FsRead`, `sysLv2FsWrite`, `sysLv2FsLSeek64`,
`sysLv2FsFStat`, `sysLv2FsFtruncate` and `sysLv2FsFsync` are static inline syscall
wrappers in the target headers, so their names are not added as external
imports. Target `nm` validation must still reject any unexplained difference;
the expected set is not generated from observed tool output.

The AUTO package-plan test retains exact default-command equality against its
historical base after narrowly adding these three members, their source files,
source search path and extension recipes to the private baseline copy. It does
not normalize away arbitrary output or move the base to the current commit.
A separate extra-member control must change that comparison. This amendment
accounts for the dependency change without admitting automatic capture as a
new default behavior.

`tests/host/ps3dk-prof-retention/dependency_test.py` is a read-only source-list
audit. It checks direct Python source groups, shell/CMake lists and SDK recipes
and has removal controls for each helper. It does not replace `-O0` host links,
both PPU ABI builds, exact archive inspection or runtime lifecycle tests.
