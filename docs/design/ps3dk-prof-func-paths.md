# Automatic PPU function paths: schema 1

An application compiled with `-finstrument-functions` and linked with the opt-in
archive `libps3dkprof_func.a` records, per registered PPU thread, a call tree of
the instrumented functions: entries and elapsed time per call path. No zone macro
is needed. This document is the wire and runtime contract for stage 1 (runtime
hooks and chunk writer). Reader decoding, symbolization and the viewer page are
later stages and do not exist yet; nothing here claims they have passed.

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
node, cursor, child time and the activation's stack frame address (the caller's
back-chain frame on the PPU). Recursion into the hooks themselves (a hook
reached while a hook of the same thread runs) is refused and counted in
`reentry`.

- Depth refusal: an entry with a full stack is not retained. The number of
  refused nested activations is kept; while it is nonzero, an exit whose frame
  is deeper than the top retained activation consumes one refused level, so an
  exit from a refused recursive call can never pop a retained frame of the same
  function. Refused entries are counted in `depth_limit` and the top retained row
  gets SELF_UNTIMED: its self time includes untimed children. An exit at or
  above the top retained frame while refused levels are outstanding is a stack
  loss (below).
- Matching: an exit matches the top retained activation only if the function is
  equal AND the exit's frame address is not above the activation's frame.
  Function equality alone cannot detect a nonlocal exit to an outer activation of
  the same recursive function; the frame check does. An entry whose frame is not
  below the top retained activation's frame (a call made from a frame the stack
  was already left by) is a stack loss as well, detected before the entry is
  attributed to a stale parent.
- Sibling calls: the frame check needs the exit hook to be called from the
  activation's own frame. GCC 16 on the PPU calls the external hook with `bl` +
  `nop` (no sibling call; the TOC must be restored). Where the hook can be
  tail-called (x86-64 host builds, or an LTO build that makes the hook local),
  the instrumented code is built with `-fno-optimize-sibling-calls`; otherwise
  every such exit is reported as a stack loss, visibly, never silently.
- Stack loss: any other exit (a longjmp, an exception unwinding through code
  without exit hooks, a mismatched frame) resets the actor's stack. All retained
  activations are dropped without charging anything after their cursors, their
  rows get TRUNCATED, `unbalanced` counts one reset, and the interval gets
  STACK_RESET. Nonlocal exits are not recovered by searching for a matching
  address. Later root-level entries of that incarnation use UNKNOWN_CALLER as
  parent and set STACK_UNKNOWN in their interval. Earlier cuts stand.
- An exit with an empty stack (a function entered before registration or before
  a reset, including the thread's own entry function) is counted in `orphan` and
  ignored. Entries before initialization are not observed; exits after the
  incarnation's retirement or stop acknowledgement are ignored.
- Dynamic stack allocation (alloca, VLAs) lowers the PPU frame address between
  entry and exit; such exits still match. A nonlocal exit that lands in the same
  activation depth with a lower frame is not detectable.

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
| 80 | u64 | unbalanced: stack resets |
| 88 | u64 | orphan: exits with an empty stack |
| 96 | u64 | deferred: cuts deferred by an unacknowledged buffer |
| 104 | u64 | reentry: hook calls refused inside a hook |
| 112 | u64 | clock: clock anomalies |
| 120 | u64 | reserved = 0 |

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
| 2 | u16 | flags: bit 0 FINAL (sealing), bit 1 SATURATED |
| 4 | u32 | reserved = 0 |
| 8 | u64 | requested epoch after this drain (the next cut request) |
| 16 | u64 | unregistered hook entries since the previous status |
| 24 | u64 | no_actor hook entries since the previous status |

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
