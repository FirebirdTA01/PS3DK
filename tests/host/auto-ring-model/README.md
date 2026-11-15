# AUTO report-ring host model

This is a proposed ownership model, an independent admission ledger and a private
decoder. It runs on a host without an SDK. It adds no SDK hooks, capture chunks,
flags or production reader behavior. Passing it establishes the stated software
invariants under supplied device events. It does not establish any RSX ordering,
addressability, cache visibility, timing accuracy or performance claim.

## Lifetimes and admission

The owner reserves a fixed destination extent, metadata for each admitted draw,
and a flush-command credit before potentially emitting work. A credit represents
space for the entire ordering/copy/publication sequence, including a partial
block. The command stream may queue many generations ahead of execution. Local
blocks are assigned cyclically; queued assignment is **not** permission to write
over the previous generation.

The scheduler supplies three distinct device facts:

1. `reports`: all report values for the executing block have landed.
2. `copy`: the transfer has finished reading that local block. Only now may its
   next generation execute. This releases local storage independently of any
   CPU observation of a label or progress by the drainer.
3. `publish`: the completed destination is visible to the consumer and carries
   the exact generation expected for that destination. A stale or future value
   cannot stand in for this fact.

`execute` is in draw-stream order. Report completion, transfer completion and
publication can be delayed independently between different blocks. A local block
still owned by an older transfer returns `LOCAL_BUSY` to the execution scheduler;
this is a modeled GPU dependency stall, not a wait imposed on owner admission.
No post-transfer label alone is assumed to prove either report-before-copy or
copy-before-reuse. The model uses generation-specific completion facts; a real
shared-label design must separately establish how it derives those facts.

The main-ring destination remains reserved through publication, drainer read and
an acknowledgement of the **entire** private encoded record. Failed, short and
oversized acknowledgements retain it unchanged for retry. This is conservative:
a future implementation could release it after copying to separately owned
storage, provided that storage has its own proved lifetime. Draining is ordered;
a later ready generation cannot bypass a pending or quarantined generation.

Frame changes, window close/open and seal flush partial blocks; unused capacity
in their reserved extent is not lent to another frame. Each block has one owner,
frame and window identity. Uncertain emission retains all already claimed
resources in quarantine. A bounded seal stops admission and returns incomplete
while any work remains; it never invents completion or frees unread work.
`reserved` counts claims, including quarantined claims, **not valid timings**.
The ledger separately conserves visits, reservations, exclusions, held metadata
and acknowledged operations after every step.

This model starts with an ordered draw stream. Replaying commands with baked
report indices or DMA destinations needs an execution-generation design that
is not provided here. `replay` is a sticky refusal and quarantines any remaining
matching block; even replay after retirement makes the run incomplete. It cannot
be treated as new measured work.

## Fixed memory and large blocks

The ring is allocated **once at attach and never grows**. The dense scenarios
default to 16 MiB. At 32 report bytes per timed draw this holds approximately
524k draws before block padding. Metadata and command credits are additional,
separately budgeted resources. Allocation failure refuses attach; fault injection
checks every allocation, partial cleanup and refusal to emit afterward.

At least two local blocks are required, so one can execute while another waits
for its copy. Block count, pairs per block, ring bytes, metadata budget, flush
credits and generation limit are parameters. The host's defensive bounds are
2..64 blocks, 1..256 pairs/block, at most 64 MiB / 65,536 destination extents,
and a non-wrapping serial limit no greater than `UINT32_MAX-1`. These are model
bounds, not proposed SDK API limits. All model storage, including scratch used
by its private encoder, is allocated at attach. The Python checker keeps its
own history and is not a simulation of target memory consumption.

The checked large-block schedules use an **illustrative** usable pool of 504
pairs. A production choice must subtract clocks, frame marks, CALL/range brackets
and any other simultaneous reservations first; 504 is not a promised draw pool.
The `--usable-pairs` parameter allows a smaller budget to be exercised.

| Local blocks | Pairs per block | Copies / ordering sequences per 500-draw frame | First blocked execution if all old copies are held | 16 MiB full-block capacity |
| --- | ---: | ---: | --- | ---: |
| 2 | 252 | 2 | Generation 3, start of next frame | 524,160 draws |
| 3 | 168 | 3 | Generation 4, start of next frame | 524,160 draws |
| 4 | 126 | 4 | Generation 5, start of next frame | 524,286 draws |

The owner admits two complete 500-draw frames before any GPU execution in these
tests, representing a GPU one frame behind. When execution catches the next
generation of a still-owned local block, it waits for that block's transfer-read
completion. The host model supplies that fact explicitly; it provides no estimate
of how many microseconds the GPU will wait. More blocks permit more outstanding
local transfers; fewer, larger blocks reduce the ordering/copy count.

Admission never waits. Exhaustion returns a counted exclusion (`RING_FULL`,
`METADATA_FULL`, `COMMAND_FULL` or `SERIAL_FULL`), distinct from execution stalls.
With a fixed ring of just one frame's extents and a paused drainer, visit 501 is
`RING_FULL`, even after all device transfers publish. A full-block workload with
a 504-draw destination ring excludes visit 505. Acknowledging the oldest block
restores admission. These small-ring cases are executed tests.

For a paused drainer and consecutive 500-draw frames, padding reduces the 16 MiB
capacity: the predicted first excluded visit is 520,001 for 2 or 3 blocks, and
520,127 for 4 blocks. This is arithmetic from the tested fixed-extent rule, not
a run that fills the entire 16 MiB ring. It assumes matching metadata/command
budgets and no earlier refusal. The generated `large-blocks.json` distinguishes
these predictions from tested events. Finite buffering removes the **fixed
per-frame pair ceiling** when device and consumer progress sustain reuse; it
does not remove finite storage, emission, serial or throughput limits.

## Hardware premises for the console probe

These claims remain **unproven**. The later probe must establish them for the
exact commands, addresses, memory mappings, ordering points and cache operations
proposed for production, with controls that detect stale or wrongly ordered data.

- **A1 — report before transfer:** every begin/end timestamp in the selected local
  report block is fully written before the transfer engine reads any of that
  generation's report bytes. The chosen ordering primitive must order the report
  writer against the transfer engine, not merely show that the front end advanced.
- **A2 — transfer read before reuse:** every transfer read of generation N's local
  report bytes finishes before a report write for the next use of that block can overwrite
  those locations. FIFO submission order and a later CPU label observation are
  not substitutes for this cross-engine dependency.
- **B1 — addressable, exact transfer:** the selected LOCAL-to-MAIN transfer can
  actually address the intended local report storage and copy its complete bytes
  into the intended, bounded main-memory destination without a fault or hang.
  Distinct generations, partial blocks, wrap and destination canaries must expose
  wrong addresses, stale values and over-copy.
- **B2 — publication and CPU visibility:** observing the selected generation's
  publication condition, with the required CPU cache/acquire operations, implies
  the entire matching destination copy is visible and stable for the drainer.
  Neither an early publication nor a stale/future generation may satisfy it.

The probe must bound its waits and include detectable broken controls for these
edges. Whether a particular label provides A1, A2 or B2 is a probe result, not a
premise of this model. Console overhead measurements are a separate later step.

## Private records and checks

The C process uses a scalar text command protocol and JSON snapshots so Python
does not mirror C struct layouts. Its decoder input is explicitly little-endian
`RGM1`: a 32-byte header, 56-byte records and a 16-byte generation/complement
commit tail. These bytes include metadata and are **not** the 32-byte report-pair
storage counted above. The encoder combines immutable admission metadata with
transferred report bytes only after publication. The decoder uses an independently
maintained admission ledger, checks exact lengths/counts/generations and scope
identity, and rejects missing, duplicate, torn, uncommitted or reordered blocks.
Synthetic timestamps expose stale copies; they are not real clock samples.

The suite checks every distinct state reachable within seven actions for a tiny
two-slot configuration (including refused actions), all 126 legal interleavings
of two complete lifetimes at each of two block capacities, wrap/recovery,
allocation failure, partial tails, bounded seal, quarantine, replay, serial
exhaustion and a 2,000-draw frame queued before any device or consumer progress.
These are bounded schedule checks, not a proof
over arbitrary concurrency or the C memory model. Execution is deliberately
single-threaded; ASan/UBSan check host memory and arithmetic, not GPU races.

Nine compiled mutants remove the report fence, copy publication fence, local
lifetime, destination lifetime, exact-label check, ring-full reporting, whole
write acknowledgement, seal flush or replay refusal. Each must fail its named
semantic assertion; a build failure, crash, timeout or sanitizer error does not
count as a kill.

Run with a native C11 compiler and Python 3 on Linux (WSL on Windows):

```sh
bash tests/host/auto-ring-model-test.sh --out /tmp/ring-plain --mutants
bash tests/host/auto-ring-model-test.sh --out /tmp/ring-asan --sanitize
# Example with fewer usable pairs and a smaller fixed ring:
bash tests/host/auto-ring-model-test.sh --out /tmp/ring-480 --usable-pairs 480 --ring-bytes 1048576
```

Output directories must be new. They retain commands, source hashes, transition
traces, stderr, result summaries and the large-block report. Both the plain mutant
gate and ASan/UBSan gate are registered in CI.
