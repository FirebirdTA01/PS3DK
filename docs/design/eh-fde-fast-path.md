# Exception lookup without the registry lock (GCC 16 PPU)

Status: experimental. Nothing here is approved for landing; the open items are at the end.

## Why

GCC 16 builds the PPU runtime with the POSIX thread model. With it, libgcc's
frame registry takes a real mutex: a throw looks up each frame it unwinds
through `_Unwind_Find_FDE`, which locks `object_mutex` on every call, and on
LV2 that lock is a system call. Measured in RPCS3, both ABIs, on a probe that
repeatedly throws through a two-frame unwind into already-initialized frame
objects: 9 mutex locks and 9 unlocks per throw. With the single-thread model
the same probe made none. The count depends on the unwind depth and the
objects involved.

Upstream GCC already has a lock-free registry: a b-tree with optimistic
version locks (`ATOMIC_FDE_FAST_PATH`, `unwind-dw2-btree.h`). Upstream enables
it only for targets that use `dl_iterate_phdr`. The PS3 PPU target registers
frames through crtbegin instead, so the fast path is off.

## What changes

gcc-16.x patch 0074 turns the fast path on for `powerpc64-ps3-elf`
(`rs6000/t-ps3-unwind`, `-DLIBGCC_PS3_ATOMIC_FDE_FAST_PATH`), with three
target adaptations:

- **Startup reserve.** crtbegin registers the program's frames from `_init`,
  before the runtime's prioritized constructors create the heap. Until the
  runtime reports the heap ready, the b-tree takes nodes from a two-node static
  reserve instead of calling `malloc`. Reserve nodes are never freed.
- **Heap-ready flag.** `crt1.c` defines `int __ps3dk_heap_ready`, zero until
  the end of constructor priority 84 (after the heap at 83 and the syscall
  table at 84), then set once with a release store. libgcc reads it with an
  acquire load. libgcc references it strongly, so a CRT without it fails to
  link instead of reaching `malloc` too early.
- **Trees kept at exit.** `ATOMIC_FDE_KEEP_TREES` drops the destructor that
  releases the registry, so destructors that run late can still deregister and
  throw.
- **Sorted table registrations.** Once a table registration
  (`__register_frame_info_table*`, `from_array`) has been looked up and
  sorted, its `orig_data` is the table of `.eh_frame` images. Upstream's
  range computation (`get_pc_range`) classifies it as one image instead.
  Deregistering such a table after its first lookup then left its frames in
  the tree over freed sort data. 0074 walks the table, within the target
  block. This is a separate defect from the one 0077 fixes.

Measured with 0074 on the same probe (RPCS3, both ABIs): the first throw makes
1 lock and 1 unlock call, and the throws after it make none. That the first
lock is the lazy sort of a newly looked-up object comes from the source, not
from attributing the call to its caller. A frame object the program reaches
for the first time later is sorted then, under the same mutex, once.

gcc-16.x patch 0077 fixes table deregistration under `-mlp64`, independent of
0074. `__deregister_frame_info_bases` treated a BEGIN whose first 32-bit word
is zero as an empty, never-registered frame. For a table registration BEGIN is
the table, whose first element is a pointer, and under `-mlp64` on this
big-endian target the high half of every pointer below 4 GB is zero. Such a
table could never be deregistered. 0077 looks the registration up first. It
applies with or without 0074 and was validated on both registry paths.

## Policy

**Startup.** The supported startup is the runtime's own CRT, as audited: one
pre-heap registration (crtbegin), a reserve high-water of two nodes, and the
heap-ready release at the end of constructor 84. This is the audited scope,
not a guarantee about every implementation-reserved constructor. Registering
frames before the heap is ready from a custom CRT or an implementation
constructor is outside it. Running out of the reserve traps before `main`.

**Allocation failure.** After the heap is ready, a failed b-tree node
allocation is fatal (`abort`). The register functions return `void`, so there
is no way to report the failure, and a silently dropped registration would
turn a later throw into `std::terminate` or a wrong unwind. The comparison with
the list registry:

- The list registry needed no registry-node allocation to register an object
  whose storage the caller supplies.
- `__register_frame` and `__register_frame_table` already `malloc` their object
  descriptors on both paths.
- 0074 adds b-tree node allocation, and makes its failure fatal.

There is no fallback to the list registry. How this ends on
hardware (trap, hang or a system-level outcome) has not been measured yet, and
must be recorded before release.

**Custom CRTs.** A custom CRT opts in by defining `__ps3dk_heap_ready`
zero-initialized and setting it once, with a release store, only after the
allocator and the system-call and locking services it needs are usable.

**Teardown.** The registry lives for the whole process; its memory is
reclaimed by process exit. This covers the process only. Module unload is not
covered.

**Modules.** C++ exceptions within or across PRX modules are not supported by
the shipped module runtime, with or without 0074:

- A PRX is linked with `-nostartfiles` and has no crtbegin, so nothing
  registers its frames. It carries its own registry and its own `malloc`, and
  the host registry never sees its frames.
- With the shipped build rule, `prx-gen` already refused a C++ module that
  uses exceptions, because of unresolved weak references.
- With 0074 the same module now fails earlier, at link time, on
  `__ps3dk_heap_ready`.

No dummy heap-ready definition is added to the PRX runtime. In the two
audited exception probes, no working capability is lost. The consequences at
run time are inferred from the links, not observed. Module exception support
is separate design work.

## Evidence (RPCS3; hardware pending)

- Registry row `eh-fde-registry`, both ABIs, on the candidate (0074 + 0077),
  0077 alone, and the unmodified baseline. The row covers:
  - startup and teardown throws, and concurrent throws;
  - registration churn and node growth;
  - every registration route, before and after a lookup, over two rounds;
  - empty frames.
- Baseline `-mlp64` fails the table-deregistration routes (the 0077 bug). The
  row retires a route after a failed removal, so the failure stays a clean red.
- Lazy sort under contention. One reader holds the object mutex in a wrapped
  lock until the other readers have reached it: four entries, one holder, and
  every lookup correct. Readers that look up the published object afterwards
  take no lock with 0074, and one lock per lookup on the list path. That one
  thread sorts and the others then find the object sorted is derived from the
  source, not instrumented.
- Fault probes:
  - reserve exhaustion traps before `main`;
  - a post-heap allocation failure dies after the probe arms;
  - their controls survive.
- The full regression set on the candidate.

## Open

- Hardware runs of the candidate.
- The hardware outcome of a fatal allocation failure.
- Landing approval.
