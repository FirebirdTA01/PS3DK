# Automatic PPU function paths: schema 1

Archives (explicit goals in `sdk/libps3dkprof/func.mk`, never part of `all`):
`func` builds `libps3dkprof_func.a` (manual lifecycle) and `func-ext` builds
`libps3dkprof_ext_func.a` (the SPU-lane and GPU-domain extension with its PROFILE
identity member), each for ILP32 and LP64. An extension app links
`libps3dkprof_ext_live.a` (unchanged) and `libps3dkprof_ext_func.a` in place of
`libps3dkprof_ext.a`. There is no automatic-lifecycle (`libps3prof`) variant yet.

An application compiled with `-finstrument-functions` and linked with the opt-in
archive `libps3dkprof_func.a` records, per registered PPU thread, a call tree of
the instrumented functions: entries and elapsed time per call path. No zone macro
is needed. This document is the wire and runtime contract for stage 1 (runtime
hooks and chunk writer) and stage 2 (the reader in `tools/ps3dk-prof`,
`ps3dk-prof paths --functions`, `--elf` symbolization and the viewer's
Functions source on the Hot paths page; see "Reader checks" at the end).

Elapsed time is timebase wall time between two observed hook events of the same
thread. It includes waits, preemption, descheduling and the cost of the hooks
themselves. It is not CPU running time. Hook cost is not subtracted.

Only instrumented translation units are observed. Library and SDK code that is not
instrumented appears as self time of its instrumented caller.

## Capability and wire identity

File flag bit 5 (value 32) enables chunk types 28, 29 and 30. It has no
dependency on domain extensions, a PROFILE, waits or frame checkpoints. Bits 0
through 4 and chunk types 1 through 26 keep their existing meanings, and type 27
stays reserved for scope declaration. Without bit 5, types 28 through 30 remain
unknown chunks. A reader that does not know bit 5 refuses the file flag instead
of misreading the chunks. A capture made with a default archive does not set bit
5 and contains none of these chunks; default archives are unchanged.

| Type | Name | Count |
| --- | --- | --- |
| 28 | FUNC_CONFIG | exactly one, before the first FUNC_TREE or FUNC_STATUS |
| 29 | FUNC_TREE | one per actor per emitted interval, plus no-data status rows |
| 30 | FUNC_STATUS | one per drain, plus one FINAL at sealing |

All integers are little-endian. Unknown schemas, unknown flag bits and nonzero
reserved fields refuse. Every chunk fits one ordinary chunk payload (the node cap
bounds a FUNC_TREE to less than 1 MiB); a cut is never split across chunks, so a
whole FUNC_TREE chunk is the unit of use and a torn one contributes nothing.

## What the hook receives (function identity)

`fn` is the raw first argument GCC passes to `__cyg_profile_func_enter/exit`,
zero-extended to 64 bits. On the PPU with GCC 16 this is the address of the
function's compact 8-byte `.opd` descriptor (`.long entry, .TOC.`) in BOTH the
ILP32 and the LP64 data models (measured with an assembly probe at -O2; not a
universal claim about every optimization level). It is not the entry address.
A reader resolves `fn` through the `.opd` section of an identified executable for
both ABIs, and keeps the raw address when the ELF does not match or resolution
fails. The runtime never dereferences `fn`. The call-site argument is not
recorded. FUNC_CONFIG field `fn_kind` = 1 names this rule; on the host test
harness the value is a plain code address.

## Actors, nodes and paths

An actor is one core thread incarnation (from an accepted THREADS entry) whose
slot index is below `max_actors`. Storage is per slot, reset when the slot is
reused, and retained after retirement until the drainer has written that
incarnation's FINAL chunk. Hook calls from a thread with no live incarnation are
counted as `unregistered` (auto_register is honoured first); calls from an
incarnation whose slot has no actor storage are counted as `no_actor`. Both are
capture-level counts in FUNC_STATUS.

A node is identified per incarnation by a u32 index into that actor's table and
is immutable once defined. Each node is defined exactly once, in the FUNC_TREE
chunk whose cut first includes it, before or in the same chunk as any row that
uses it. A row referring to an undefined node refuses.

- Node 0 is the reserved LOST node (`parent` = 0xFFFFFFFD, `fn` = 0). It is
  defined in the actor's first data chunk. It holds calls and time whose call
  path is unknown: LOST PARENT ATTRIBUTION. It is never a child of any node and
  must never be presented as a child path. Entries go to it when the table is
  full or the bounded probe fails, and every entry made while a LOST activation
  is open goes to it too (nested LOST activations count calls but add no time,
  so LOST time is not counted twice).
- Nodes 1 .. node_capacity-1 are `(parent, fn)` pairs. `parent` is another node
  index of the same incarnation, or ROOT (0xFFFFFFFF: entered with no observed
  instrumented activation open, before any stack reset), or UNKNOWN_CALLER
  (0xFFFFFFFE: entered with an empty stack after a stack reset, so the true
  callers are unknown). A path is the chain of parents up to ROOT or
  UNKNOWN_CALLER.

The node index is found by bounded linear probing (`max_probe` slots) in a hash
table of `hash_slots` entries (a power of two, at least twice the capacity). A
probe that finds neither the key nor a free entry within the bound is a refusal:
the entry goes to the LOST node and is counted in `lost`.

## Interval accounting

Each actor accumulates, per node, for the current interval: `calls` (entries:
one per observed function entry, counted when the entry happens), `incl` (elapsed
ticks inside activations of the node) and `child` (elapsed ticks of retained
child activations). Self time is `incl - child`.

Every retained activation has a cursor (`from`, initially its entry tick) and the
child time accumulated since that cursor. At a cut with tick T, every retained
open activation i (outermost first) is charged exactly once: `incl += T - from_i`
and `child += child_i + (T - from_(i+1))` when activation i+1 is a timed child.
Then every cursor advances to T and every child accumulator is zeroed. A later
exit charges only `exit - from`, so no published portion is ever charged again,
and the sum of all intervals equals the uninterrupted totals exactly. A row with
open activations at the cut carries row flag OPEN and the number of open
activations in `open`; their calls were counted in the interval of their entry.

A tick lower than a cursor is a clock anomaly: zero is charged, the anomaly is
counted, and the chunk is flagged. Accumulators and counters saturate at
2^64-1 and set SATURATED; they never wrap.

## Cuts, snapshots and acknowledgement

There is no shared mutable accumulator. The owner thread owns its tables. The
handoff is:

1. The drainer bumps the capture epoch (atomic, release) at the end of each drain.
2. At its next hook entry or exit, the owner reads the epoch. If it is newer than
   the last epoch it published and the drainer has acknowledged that previous
   buffer (acquire load of the ack word), the owner takes the cut at that hook's
   own tick: it charges the open activations, writes the new node definitions
   and the nonzero rows into its own publish buffer, zeroes the interval, and
   publishes the epoch with a release store.
3. If the previous buffer is not yet acknowledged, the owner never overwrites it:
   it keeps accumulating into the live interval and counts one deferred cut for
   that epoch (`deferred`). The next cut covers the whole longer interval.
4. The drainer reads a buffer only after an acquire load shows a newer epoch,
   writes it as a FUNC_TREE chunk, and acknowledges it (release) only when the
   sink accepted the whole chunk. A short or failed sink write leaves it
   unacknowledged; since a sink failure is terminal, nothing later can refer to a
   definition absent from the file.

Each data chunk carries its own interval number (1, 2, ... per incarnation), its
start tick (the previous cut tick, or the registration tick for interval 1), and the
owner's cut tick. A drain request is not a frame cut on another thread, and no
drainer tick is recorded: interval numbers alone do not associate an interval
with an application frame. An answered epoch that skips requested epochs means
the requests were coalesced into one interval.

A thread that has not reached a hook since the request is reported in that drain
as a no-data FUNC_TREE with LAGGING, never as zero activity. A data chunk whose
answered epoch is older than the drain's requested epoch is also LAGGING.

At retirement (`ps3dk_prof_thread_end`) or the stop acknowledgement, the owner
stamps a final tick before the core's release of its ack/retire word. After the
acquire of that word the drainer owns the whole actor: it first writes any
unacknowledged published buffer, then takes the final cut itself at the stamped
tick and writes it with FINAL. Activations still open then are charged to the
final tick and flagged OPEN. An incarnation that never acknowledged (NOT_QUIESCED
at sealing) gets any buffer it had already published, then a no-data chunk with
INCOMPLETE_TAIL: its tail after the last acknowledged cut is unknown.

The epoch starts at 0 and the first drain requests epoch 1, so a drain writes the
cuts answering the previous drain's request. A thread registered after a request
is not asked for it (no LAGGING row for an epoch older than its registration).
Recursion into the hooks is refused per thread (`reentry`); a refused call's
elapsed time stays inside the activation that was running when it happened.

## Stack loss (conservative coverage)

The shadow stack holds `stack_depth` retained activations, each with its function,
node, cursor, child time and the activation's physical stack frame address (the
instrumented function's back-chain frame on the PPU, its frame pointer on the
host). Frames grow downward. Recursion into the hooks themselves (a hook reached
while a hook of the same thread runs) is refused and counted in `reentry`.

GCC instruments inline expansions too, and an inline expansion runs in its
container's physical frame. A frame EQUAL to the top activation's frame is
therefore valid nesting (an inline child), and only a strictly shallower frame
shows that the stack was left.

- Entry: an entry whose frame is strictly above the top retained activation's
  frame means the stack was abandoned (longjmp, unwinding without exit hooks).
  Every retained activation strictly deeper than the new frame is dropped, the
  rest are kept, and the entry is attributed to the new top.
- Exit: an exit matches the top retained activation when the function is equal
  and the exit's frame is not above the activation's frame (dynamic stack
  allocation may lower the PPU frame of the top activation). Otherwise the
  nearest retained activation below with the same function AND the same frame is
  the one exiting: everything above it is dropped and it is charged normally.
  When no retained activation matches, the whole stack is dropped.
- Dropping: a dropped activation is charged nothing after its cursor (its row
  gets TRUNCATED), `unbalanced` counts the event, `abandoned` counts every
  dropped activation and refused level, and the interval gets STACK_RESET. The
  surviving top's row gets SELF_UNTIMED (its self time now holds untimed child
  time). Earlier cuts stand. When the stack is emptied by a drop, later
  root-level entries of that incarnation use UNKNOWN_CALLER as parent and set
  STACK_UNKNOWN in their interval.
- Depth refusal: an entry with a full stack is not retained. The first refused
  level's frame and function are kept with the number of refused levels. While it
  is nonzero, an entry at or below that frame is one more refused level and an
  exit at or below it consumes one (the last one must be the first refused
  function), so an exit from a refused recursive call, or from an inline leaf
  refused at the deepest retained level, never pops a retained frame. Anything
  else left the refused region: its levels are dropped and the event is matched
  against the retained stack as above. Refused entries are counted in
  `depth_limit` and the top retained row gets SELF_UNTIMED.
- An exit with an empty stack (a function entered before registration, including
  the thread's own entry function) is counted in `orphan` and ignored. Entries
  before initialization are not observed; exits after the incarnation's
  retirement or stop acknowledgement are ignored.

Stated limits (not detectable from function and frame alone):

- Re-entry at an equal frame after a nonlocal exit: a function called from the
  landing frame usually occupies the physical frame of the abandoned activation
  that was called from there before. It is indistinguishable from an inline
  child of that activation, so the abandoned activation is kept and the new call
  is attributed under it until a later exit exposes the loss.
- A nonlocal exit that lands in the same activation depth with a lower frame, or
  a dynamically allocating activation that is not the top when it exits, cannot
  be matched exactly and is treated as a drop.

Hooks and sibling calls: the frame check needs the exit hook to be called from
the activation's own frame. With the external hook, GCC 16 calls it with `bl` +
`nop` before the frame is released in both PPU ABIs. A hook visible to LTO is
tail-called after SP is restored, in both ABIs, which breaks every exit. So the
hooks stay external: the function-path archives are never built with LTO (the
build refuses any member carrying `.gnu.lto_` sections) and are never part of the
application's LTO unit. An instrumented application built with LTO also uses
`-fno-optimize-sibling-calls`. The x86-64 host test builds its workload with
`-fno-optimize-sibling-calls` because that compiler tail-calls the external exit
hook of a void function.

## FUNC_CONFIG (28)

48 bytes:

| Offset | Type | Meaning |
| --- | --- | --- |
| 0 | u16 | schema = 1 |
| 2 | u16 | fn_kind = 1 (raw hook argument; PPU: `.opd` descriptor address, both ABIs) |
| 4 | u32 | pointer bytes of the producing ABI (4 or 8) |
| 8 | u32 | max_actors |
| 12 | u32 | node_capacity (including the LOST node) |
| 16 | u32 | stack_depth |
| 20 | u32 | hash_slots |
| 24 | u32 | max_probe |
| 28 | u32 | reserved = 0 |
| 32 | u64 | timebase frequency (ticks per second; same clock as zones) |
| 40 | u64 | allocated bytes (all actor storage, publish buffers and hash tables) |

## FUNC_TREE (29)

Header, 128 bytes, then `n_nodes` definitions of 16 bytes, then `n_rows` rows of
40 bytes. The length is exactly `128 + 16 n_nodes + 40 n_rows`.

| Offset | Type | Meaning |
| --- | --- | --- |
| 0 | u16 | schema = 1 |
| 2 | u16 | flags (below) |
| 4 | u32 | incarnation |
| 8 | u64 | interval number (0 in a no-data chunk) |
| 16 | u64 | answered epoch (0 in a no-data or FINAL chunk: the drainer took that cut) |
| 24 | u64 | requested epoch at emission |
| 32 | u64 | interval start tick (0 in a no-data chunk) |
| 40 | u64 | owner cut tick (0 in a no-data chunk) |
| 48 | u32 | n_nodes |
| 52 | u32 | n_rows |
| 56 | u32 | retained activations open at the cut |
| 60 | u32 | refused depth levels outstanding at the cut |
| 64 | u64 | depth_limit: entries refused by the stack depth |
| 72 | u64 | lost: entries charged to the LOST node |
| 80 | u64 | unbalanced: stack-loss events |
| 88 | u64 | orphan: exits with an empty stack |
| 96 | u64 | deferred: cuts deferred by an unacknowledged buffer |
| 104 | u64 | reentry: hook calls refused inside a hook |
| 112 | u64 | clock: clock anomalies |
| 120 | u64 | abandoned: activations and refused levels dropped by stack loss |

Counters are per-interval deltas; their sum over an incarnation's chunks is its
total. Chunk flags: bit 0 DATA (a cut is present), bit 1 LAGGING, bit 2 FINAL,
bit 3 INCOMPLETE_TAIL, bit 4 STACK_RESET, bit 5 STACK_UNKNOWN, bit 6
DEPTH_LIMITED, bit 7 LOST, bit 8 SATURATED, bit 9 OPEN, bit 10 CLOCK. A no-data
chunk has DATA clear, no definitions or rows, and zero counters.

Definition: u32 node, u32 parent, u64 fn.

Row: u32 node, u16 row flags, u16 reserved = 0, u32 open, u32 reserved = 0,
u64 calls, u64 incl, u64 child. Row flags: bit 0 OPEN, bit 1 SELF_UNTIMED, bit 2
LOST (node 0 only), bit 3 SATURATED, bit 4 TRUNCATED. `child <= incl` holds for
every row.

Ordering: per incarnation, data chunks have consecutive interval numbers from 1,
strictly increasing nonzero answered epochs (FINAL excepted), start tick equal to
the previous cut tick, and at most one
FINAL or INCOMPLETE_TAIL chunk ends the incarnation. No chunk for an incarnation
follows its RETIRED entry's drain.

## FUNC_STATUS (30)

32 bytes, after the drain's FUNC_TREE chunks:

| Offset | Type | Meaning |
| --- | --- | --- |
| 0 | u16 | schema = 1 |
| 2 | u16 | flags: bit 0 FINAL (sealing), bit 1 SATURATED (sticky, below) |
| 4 | u32 | reserved = 0 |
| 8 | u64 | requested epoch after this drain (the next cut request) |
| 16 | u64 | unregistered hook entries since the previous status |
| 24 | u64 | no_actor hook entries since the previous status |

The capture counters `unregistered` and `no_actor` are saturating: an increment
is at most four compare-and-swap attempts. At 2^64-1, or when contention exhausts
the attempts, the counter stops being exact, and from then on EVERY FUNC_STATUS,
the FINAL one included, sets SATURATED: the deltas are lower bounds. A delta
never wraps. The per-thread hook-reentry tally saturates at 2^32-1 and sets the
chunk's SATURATED when it does.

Exhaustion rule: the requested epoch never passes 2^64-2. From then on no further
cut is requested (owners keep accumulating into their FINAL chunk) and every
FUNC_STATUS sets SATURATED. An interval number is at most the number of answered
epochs plus the FINAL, so it cannot pass that bound.

## Memory bounds and configuration

`ps3dk_prof_func_configure` (before init) sets `max_actors` (default the smaller
of max_threads and 8), `node_capacity` (default 4096, 4 to 16384), `stack_depth`
(default 256, 4 to 256) and `max_bytes` (default 8 MiB). Per actor the runtime
allocates the stack, the node table, the interval rows, the dirty list, the hash
table and a publish buffer sized for a full cut; the total, computed with
overflow checks, must not exceed `max_bytes` or initialization fails with
EINVAL. All of it is one allocation at init: nothing is allocated, locked or
placed in thread-local storage per call. The only thread-local addition is a
reentry flag.

## Reader checks (stage 2)

The reader (`tools/ps3dk-prof/src/func_paths.rs`) decodes each chunk whole and
keeps nothing from a chunk that breaks a rule; every refusal is MALFORMED with
rule FuncConfig, FuncTree or FuncStatus. Besides the lengths, schemas, reserved
fields and known flag bits above, it refuses:

- FUNC_CONFIG: a second one; `fn_kind` other than 1; pointer bytes that differ
  from the HEADER abi (1: 4, 2: 8); `max_actors` outside 1 to the smaller of
  1024 and `max_threads`; `node_capacity` outside 4 to 16384; `stack_depth`
  outside 4 to 256; `hash_slots` not a power of two of at least twice the
  capacity; `max_probe` 0; a timebase other than the CLOCKS frequency; zero
  allocated bytes.
- FUNC_STATUS: one before FUNC_CONFIG; an ordinary status whose requested epoch
  is not the previous request plus one (the first is 1), or a FINAL one that
  does not repeat the current request; any function-path chunk after the FINAL
  status.
- FUNC_TREE, every chunk: one before FUNC_CONFIG; an undeclared incarnation, one
  after its RETIRED entry, or one whose slot is at or above `max_actors`; a
  requested epoch other than the current request (0 before the first status); a
  chunk after the incarnation's FINAL or INCOMPLETE_TAIL chunk.
- No-data chunks: flags other than LAGGING and/or INCOMPLETE_TAIL (at least one),
  any nonzero cut field or counter, or LAGGING alone for requested epoch 0.
- Data chunks: INCOMPLETE_TAIL; a FINAL chunk with an answered epoch or LAGGING;
  a non-FINAL chunk whose answered epoch is 0, not above the previous one or
  above the requested epoch, or whose LAGGING flag differs from `answered <
  requested`; an interval number that is not the previous plus one; a start tick
  other than the previous cut tick; LOST, STACK_RESET, CLOCK, DEPTH_LIMITED and
  OPEN disagreeing with, in order, `lost > 0`, `unbalanced > 0`, `clock > 0`,
  `depth_limit > 0 or refused levels > 0`, and `open > 0`; STACK_UNKNOWN before
  any STACK_RESET of the incarnation; more open activations than `stack_depth`,
  or refused levels with a stack that is not full.
- Definitions: a first data chunk without the LOST node; a node index that is
  not the next one; more nodes than `node_capacity`; node 0 with another parent
  or a nonzero `fn`; any other node whose parent is the LOST marker, the LOST
  node, or a node not yet defined; an UNKNOWN_CALLER parent without a stack reset
  of the incarnation and STACK_UNKNOWN on the chunk; a repeated `(parent, fn)`.
- Rows: an undefined node; a second row for one node; unknown row flags or a
  nonzero reserved field; `child > incl`; the LOST row flag on any node but 0
  (or missing on node 0); OPEN disagreeing with `open > 0`; TRUNCATED,
  SELF_UNTIMED, SATURATED or OPEN without STACK_RESET, DEPTH_LIMITED, SATURATED
  or OPEN on the chunk; open counts whose sum is not the header's open count.

Whole-capture totals per node are exact sums (`u128`) of the rows. The rows of
each interval are retained within a reader budget; past it the per-interval
views are refused visibly and the totals stay exact. A thread with no data
chunk, a missing interval number and every no-data chunk are reported as
UNKNOWN, never as zero. A cut tick below its start tick is not refused (no rule
forbids it); that interval's length is UNKNOWN.
