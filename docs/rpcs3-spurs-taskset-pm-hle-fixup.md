# RPCS3 HLE fix-up: complete the Spurs Taskset PM SPU side

> **Audience:** a Claude Code session running in the RPCS3 source tree
> (`/home/firebirdta01/source/repos/RPCS3`).  This document is
> self-contained.  It describes a known-broken HLE path, the contract
> the SPU code expects of it, and the concrete steps to wire it up.
>
> **Goal:** make the SPURS taskset policy-module syscall path
> (`spursTasksetSyscallEntry` →  `spursTasksetProcessSyscall`)
> functional, so that user-mode SPU tasks calling
> `cellSpursSemaphoreP` / `cellSpursSemaphoreV` / `cellSpursYield` /
> `cellSpursWaitSignal` / `cellSpursReceiveWorkloadFlag` actually
> block-and-wake instead of returning early or hanging.

## TL;DR

In the RPCS3 tree, **`rpcs3/Emu/Cell/Modules/cellSpursSpu.cpp`**:

- `spursTasksetSyscallEntry` (line ~1352): function body literally
  contains `fmt::throw_exception("Broken (TODO)")`.  It is the only
  caller of `spursTasksetProcessSyscall`, so the entire taskset-PM
  syscall path is dead.
- HLE registration of taskset-PM entry points is commented out at
  lines ~643, 1337, 1338 — the kernel never installs HLE hooks at
  `0xA00` (TASKSET_PM_ENTRY) or `0xA70` (TASKSET_PM_SYSCALL).
- Workload code at `SPURS_IMG_ADDR_TASKSET_PM` (sentinel 0x200) is
  not loaded into LS at all (see line ~642's switch, no `memcpy`
  branch).

In **`rpcs3/Emu/Cell/Modules/cellSpurs.cpp`**:

- `_cellSpursSemaphoreInitialize` is `UNIMPLEMENTED_FUNC` returning
  `CELL_OK`; the in-main-memory CellSpursSemaphore cache line
  remains BSS-zeroed, so the SPU side reads count == 0 on the first
  `P` regardless of the requested initial total.
- `cellSpursSemaphoreGetTasksetAddress` is also `UNIMPLEMENTED_FUNC`.

The SPU side of all this is shipped (and byte-equivalent to the
upstream `libspurs.a`) in the PS3 Custom Toolchain repo — see
`sdk/libspurs_task/src/spurs_*.S`.  When you fix the RPCS3 side,
the existing `samples/spurs/hello-spurs-semaphore` sample in that
tree will exercise the round-trip end-to-end.

## How a sync primitive flows from SPU through the kernel

Call site (user task, on SPU):
```
cellSpursSemaphoreP(eaSemaphore)
  ├── _cellSpursTaskCanCallBlockWait()          ; gate, returns 0 on OK
  ├── GETLLAR/PUTLLC retry loop on cache line @ eaSemaphore
  │   ├── if count > 0:  count--, commit, return
  │   └── if count == 0: queue self in waiter list, commit, then ...
  ├── _cellSpursTaskCanCallBlockWait()          ; second gate before BLOCK
  └── dispatch (BLOCK service):
      r3 = 2  (service id = WAIT_SIGNAL)
      bisl  $0, *(LS dispatch_base + 0xc4)
      ; control returns to next instruction only after V wakes us.
```

The function pointer at `LS dispatch_base + 0xc4` is the per-task
"service entry" — for tasks running under a Class-0 Taskset PM,
**dispatch_base = LS 0x2700** (the `SpursTasksetContext`), and the
4-byte field at offset `+0xc4` (= `syscallAddr` in the RPCS3 struct)
is set to `CELL_SPURS_TASKSET_PM_SYSCALL_ADDR = 0xA70`.

So `bisl $0, *(LS 0x27c4)` jumps the SPU to **LS 0xA70**, which is
where the taskset-PM's own syscall trampoline lives in the upstream
SPRX firmware.  In a working HLE setup, this is exactly where
RPCS3 installs an HLE hook that diverts to
`spursTasksetSyscallEntry`.

## The five syscalls that need to work

`spursTasksetProcessSyscall` already has switch arms for these
(lines ~1885+); the bodies look approximately right but are
unreachable from the dead `spursTasksetSyscallEntry`.  Service-id
constants are in `cellSpurs.h`:

| Service ID | Constant                              | Wakes on... |
|-----------:|---------------------------------------|-------------|
| 0          | `CELL_SPURS_TASK_SYSCALL_EXIT`        | n/a (noreturn) |
| 1          | `CELL_SPURS_TASK_SYSCALL_YIELD`       | priority change |
| 2          | `CELL_SPURS_TASK_SYSCALL_WAIT_SIGNAL` | `cellSpursSendSignal` to this task |
| 3          | `CELL_SPURS_TASK_SYSCALL_POLL`        | n/a (returns immediately) |
| 4          | `CELL_SPURS_TASK_SYSCALL_RECV_WKL_FLAG` | `cellSpursWorkloadFlagReceiver` |

Each switch arm calls `spursTasksetProcessRequest(spu,
SPURS_TASKSET_REQUEST_*, ...)` which already exists and looks
correct (see lines ~1418+).  Once the entry is wired up, only
`spursTasksetSyscallEntry`'s `Broken (TODO)` body needs replacing.

## What spursTasksetSyscallEntry must do

The intent (already half-coded at line 1352) is:

```cpp
bool spursTasksetSyscallEntry(spu_thread& spu)
{
    auto ctxt = spu._ptr<SpursTasksetContext>(0x2700);

    // 1. Save the calling task's volatile state (LR, SP, $80..$127)
    //    into ctxt->savedContext{Lr,Sp,R80ToR127} so a later
    //    spursTasksetResumeTask can restore it.
    ctxt->savedContextLr = spu.gpr[0];
    ctxt->savedContextSp = spu.gpr[1];
    for (auto i = 0; i < 48; i++)
        ctxt->savedContextR80ToR127[i] = spu.gpr[80 + i];

    // 2. Dispatch the syscall.  Return value goes back in r3.
    spu.gpr[3]._u32[3] =
        spursTasksetProcessSyscall(spu, spu.gpr[3]._u32[3], spu.gpr[4]._u32[3]);

    // 3. If the syscall did NOT block (didn't take the
    //    spursTasketSaveTaskContext path), resume the calling task in
    //    place.  If it DID block, do not resume - control returns to
    //    the kernel scheduler which picks the next runnable task.
    //
    // The `if (spu.m_is_branch == false)` predicate was the original
    // intent (line 1369-1371 of the existing TODO body).  RPCS3
    // doesn't have an `m_is_branch` flag exactly; use whatever signal
    // the existing yield/exit paths use to indicate "context was
    // saved & control was transferred elsewhere".  Suggested:
    //
    //   - If `spursTasksetProcessSyscall` returned a value indicating
    //     "blocked" (e.g. spursTasksetProcessRequest returned 0 on
    //     WAIT_SIGNAL and SaveTaskContext was called), return without
    //     restoring.  The kernel scheduler runs the next task.
    //   - Otherwise, call `spursTasksetResumeTask(spu)` to restore
    //     the calling task's state.
    return false;
}
```

`spursTasksetResumeTask` (line 1379) already handles the restore
side correctly: copy `savedContextLr/Sp/R80ToR127` back into
`spu.gpr` and set `spu.pc = savedContextLr._u32[3]`.

The "did this syscall block" predicate is the subtle part.  Look at
how the existing dead-but-coded switch arms call
`spursTasketSaveTaskContext`:

- `WAIT_SIGNAL`: only saves context if
  `spursTasksetProcessRequest(SPURS_TASKSET_REQUEST_POLL_SIGNAL)`
  returned 0 (= no signal pending).  When the signal IS pending,
  it returns immediately.
- `RECV_WKL_FLAG`, `YIELD`: similar — context save only happens on
  the blocking path.

So the cleanest signal is "did `spursTasketSaveTaskContext` get
called?".  Either:

1. Have `spursTasketSaveTaskContext` set a flag in `ctxt` (e.g.
   `ctxt->task_yielded = 1`) and check it after
   `spursTasksetProcessSyscall` returns, OR
2. Have `spursTasksetProcessSyscall` return a sentinel (e.g.
   `INT_MIN`) when the task blocked and check for it.

## Setting up the HLE hooks

`spursTasksetSyscallEntry` won't be called unless RPCS3 hooks LS
address `0xA70` to it.  The commented-out lines that need
re-enabling:

In `spursKernelDispatchWorkload` (around line 643):
```cpp
case SPURS_IMG_ADDR_TASKSET_PM:
    spu.RegisterHleFunction(0xA00, spursTasksetEntry);
    break;
```

In `spursTasksetEntry` (around line 1337-1338):
```cpp
spu.UnregisterHleFunctions(CELL_SPURS_TASKSET_PM_ENTRY_ADDR, 0x40000);
spu.RegisterHleFunction(CELL_SPURS_TASKSET_PM_ENTRY_ADDR, spursTasksetEntry);
spu.RegisterHleFunction(ctxt->syscallAddr, spursTasksetSyscallEntry);
```

`ctxt->syscallAddr` is `CELL_SPURS_TASKSET_PM_SYSCALL_ADDR = 0xA70`.

Check what `spu_thread::RegisterHleFunction` looks like in the
current RPCS3 codebase — the API may have evolved since these
lines were originally written.  Search for any existing
`RegisterHleFunction` usage that's still active for similar PM
hooks (e.g. JobChain PM path).

## SemaphoreInitialize implementation

The SPU side reads from a 128-byte cache line in main memory.  The
PPU side `_cellSpursSemaphoreInitialize(spurs, taskset, semaphore,
total)` must populate that line with the right initial bytes.
Right now it returns `CELL_OK` without writing anything, so the
cache line is BSS-zero.

The SPU code we hand-ported (see `sdk/libspurs_task/src/spurs_semaphore_p.S`
in the PS3 Custom Toolchain repo) tells you the layout it expects:

| Offset | Bytes | Meaning |
|-------:|------:|---------|
|  +0x00 |   16  | counter (signed 32-bit at +0); SemaphoreP reads this on GETLLAR |
|  +0x10 |   16  | waiter list head (16-bit task-id list, packed) |
|  +0x20 |   16  | waiter list info |
|  +0x30 |   16  | bound-taskset / bound-spurs EA + flags (SemaphoreV at offset 0xb0 LSA reads `+8` here for taskset ptr null-check) |
|  +0x40 |   32  | kernel-side scratch |
|  +0x60 |   16  | originating-Spurs `SpursAddress` snapshot |
|  +0x70 |   16  | reserved / state bits |

Initialise:
```cpp
s32 _cellSpursSemaphoreInitialize(vm::ptr<CellSpurs> spurs,
                                  vm::ptr<CellSpursTaskset> taskset,
                                  vm::ptr<CellSpursSemaphore> semaphore,
                                  s32 total)
{
    if (!semaphore)         return CELL_SPURS_TASK_ERROR_NULL_POINTER;
    if (!semaphore.aligned(16)) return CELL_SPURS_TASK_ERROR_ALIGN;
    if (total < 0)          return CELL_SPURS_TASK_ERROR_INVAL;
    // (Refuse if BOTH taskset and spurs are null, or BOTH non-null.)
    if (!taskset && !spurs) return CELL_SPURS_TASK_ERROR_NULL_POINTER;
    if ( taskset &&  spurs) return CELL_SPURS_TASK_ERROR_INVAL;

    auto sem = vm::_ptr<u8>(semaphore.addr());
    std::memset(sem, 0, 128);

    // count
    *reinterpret_cast<be_t<s32>*>(sem + 0x00) = total;

    // bound-taskset or bound-spurs at +0x30 (likely +0x38 for the EA;
    // verify by examining what cellSpursSemaphoreGetTasksetAddress
    // reads back - it returns the value at the same offset.  A
    // disasm of semaphoreGetTasksetAddress.o in the upstream
    // libspurs.a will confirm.
    if (taskset) {
        *reinterpret_cast<be_t<u64>*>(sem + 0x30 + 8) = taskset.addr();
        // mark "task-bound" via flag bit somewhere in the +0x30 quadword
    } else {
        *reinterpret_cast<be_t<u64>*>(sem + 0x30 + 8) = spurs.addr();
        // mark "IWL" via the alternate flag
    }

    // The originating-Spurs snapshot at +0x60 is what
    // _cellSpursGetWorkloadFlag reads on the SPU; we need the EA
    // of the CellSpurs that owns this semaphore.  For task-bound
    // semaphores, dereference taskset->spurs.  For IWL, use spurs.
    auto owningSpurs = taskset ? +taskset->spurs : spurs;
    *reinterpret_cast<be_t<u64>*>(sem + 0x60) = owningSpurs.addr();

    return CELL_OK;
}
```

The exact bit positions of the "task-bound" / "IWL" flags need to
be confirmed by reading the SPU disasm of
`/tmp/libspurs_ref/semaphore{Initialize,GetTasksetAddress,P,V}.o`
(extract once with `spu-elf-ar x ~/cell-sdk/475.001/cell/target/spu/lib/libspurs.a`).

`cellSpursSemaphoreGetTasksetAddress` mirrors the layout: it
reads the EA back out at the same offset and returns it through
the out-pointer.  Currently `UNIMPLEMENTED_FUNC`; pair it with the
Initialize fix.

## Verification

After fixing both:

```bash
cd /home/firebirdta01/source/repos/PS3_Custom_Toolchain
./scripts/rpcs3-run.sh \
    samples/spurs/hello-spurs-semaphore/hello-spurs-semaphore.fake.self 30
```

Expected SUCCESS-ful tty output:
```
hello-spurs-semaphore: P/V smoke test
  Spurs2::initialize ok
  Taskset::create ok
  SemaphoreInitialize ok
  Tasks launched: producer=0 consumer=1
  consumer counter = 0x00000008 (target 8)
  Taskset shutdown+join ok
  Spurs finalize ok
DONE
```

Currently the third-from-last line reads:
```
  consumer counter = 0xfa51ed0f (target 8)
```
where `0xfa51ed0f = 0xfa11ed00 | 0x8041090f`, with the
`0x8041090f` being `CELL_SPURS_TASK_ERROR_STAT` returned by the
SPU-side `_cellSpursTaskCanCallBlockWait` gate when the kernel
hasn't populated the per-task control block at LS
`dispatch_base + 0x98..0x9f` (actually it does — the failure
is one step earlier, in the gate itself, but the symptom is
identical from the user's side: SemaphoreP returns
`0x8041090f`).

You can also test the simpler non-blocking path independently by
initialising the semaphore with `total >= iters` so neither
producer nor consumer has to BLOCK.  Once `_cellSpursSemaphoreInitialize`
sets `count = total` correctly, this should pass even before the
syscall entry is fixed.

## Reference materials in the toolchain repo

- `sdk/libspurs_task/src/spurs_semaphore_p.S`,
  `spurs_semaphore_v.S`, `spurs_send_signal.S` —
  byte-equivalent SPU implementations.  Read these for the exact
  cache-line layout, the GETLLAR/PUTLLC retry pattern, the
  service-id 2 dispatch, and the waiter-list packing.

- `sdk/libspurs_task/src/spurs_can_call_block_wait.S` —
  the gate that returns `0x8041090f` if state isn't right.
  Reads LS 0x1e0+6 for `'TK'` (0x544b), expects 8 bytes at
  `dispatch_base + 0x98` to be non-zero (the
  `context_save_storage_and_alloc_ls_blocks` field), and applies
  an SP-derived bit pattern check against the `ls_pattern`
  bitmap at `dispatch_base + 0xa0`.  Once the syscall path is
  reached, this gate is satisfied because the kernel populates
  these fields during dispatch.

- `docs/abi/libspurs-sync-primitives.md` — public contract
  documentation: error codes, calling convention, caller
  obligations.

## Things to check while you're in there

`spursTasksetEntry` (line 1317) is also dead — its caller
(`spursKernelDispatchWorkload` for `SPURS_IMG_ADDR_TASKSET_PM`)
just `break`s out without invoking it because the
`spu.RegisterHleFunction(0xA00, spursTasksetEntry)` line is
commented out.  Re-enable that registration if you want fresh
taskset workloads to actually start (the existing
`hello-spurs-task` sample appears to work because the dispatch is
masked by simpler exit/yield paths that don't need the syscall
entry).

Cross-check `cellSpursBarrier{Initialize,Wait,Notify,GetTasksetAddress}`
PPU stubs — the Barrier family has the same kind of
`UNIMPLEMENTED_FUNC` gaps as Semaphore and follows the same
cache-line + atomic-DMA pattern.  Once Semaphore works, Barrier
is ~50 lines of similar code.

Likewise `cellSpursQueue{Push,Pop}{Begin,End}` and
`cellSpursEventFlag{Wait,Set,Clear,Initialize}` all walk the same
ground.

## House rules to honour

- This file is the deliverable: paste it as the opening user
  message of a fresh Claude Code session in the RPCS3 tree at
  `/home/firebirdta01/source/repos/RPCS3`, no prior context
  needed.
- The session is editing RPCS3, NOT the PS3 Custom Toolchain.
  Confirm `pwd` before any edits.
- Build RPCS3 with `cmake --build build` from the repo root after
  changes; test by running `rpcs3 --no-gui <fake.self>` against
  the toolchain's `hello-spurs-semaphore.fake.self`.
- Don't commit RPCS3 unless explicitly asked.  This is upstream
  work; PR-quality changes only, with a rationale matching this
  document.
