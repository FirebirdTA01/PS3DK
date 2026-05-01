# Next-session prompt — SPU sync primitives (semaphore / barrier / queue / event-flag wait)

## TL;DR

The libspurs SPU foundational surface is shipped (17 entry points
across two sessions: 5 module getters + 6 dispatch helpers + 6
event-port lv2-syscalls).  The remaining gap is the **task-side
synchronization primitives** in libspurs.a (reference SDK):

| Primitive family | Functions | Byte size each | Total |
|---|---|---:|---:|
| Semaphore | `cellSpursSemaphoreP`, `cellSpursSemaphoreV` | 480 / 432 | 912 |
| Barrier   | `cellSpursBarrierWait`, `cellSpursBarrierNotify` | 736 / 760 | 1496 |
| Queue     | `cellSpursQueuePushBegin`, `PushEnd`, `PopBegin`, `PopEnd` | 1184 / 904 / 1184 / 984 | 4256 |
| LFQueue   | `cellSpursLFQueuePush`, `Pop` | similar | ~2000 |
| EventFlag | `cellSpursEventFlagWait` | 1400 | 1400 |
| Helpers (deps) | `_cellSpursTaskCanCallBlockWait`, `cellSpursSendSignal`, `_cellSpursSendWorkloadSignal`, `_cellSpursGetWorkloadFlag` | 152 / 464 / 168 / 48 | 832 |

**~11 KB of careful SPU asm** with atomic GETLLAR/PUTLLC retry loops,
DMA cycles, and dispatcher BLOCK service-calls.  Each primitive has
its own subtle wire protocol with the SPRX kernel.  Realistic scope:
**one primitive family per session** (semaphore first, smallest).

This is genuinely reverse-engineering work — not a quick port.  Plan
2-3 sessions for full sync-primitive parity.

## What's done (anchor against this state)

Latest commits:
```
3cf6156 sdk: cellSpursModuleExit/Poll/PollStatus + Poll + GetElfAddress + GetIWLTaskId
3a53713 sdk: libsputhread sys_spu_thread event-port + yield + switch syscalls
d30cd42 sdk: cellSpursGet{SpursAddress,CurrentSpuId,TagId,WorkloadId,SpuCount}
b87617e sdk: fix hello-spu-job halt; emit "bin2" magic at LS 0x20
```

In `sdk/libspurs_task/`:
- 11 SPU runtime helpers (5 module-info getters + 6 dispatch-call
  helpers + 6 task-side helpers carried over from earlier sessions)
- `<cell/spurs/common.h>` declares all 11
- Makefile uses `-fno-schedule-insns2` (REQUIRED — see below)

In `sdk/libsputhread/`:
- All 11 reference-SDK lv2-syscall wrappers
  (event_flag_set_bit / set_bit_impatient / spu_thread_exit /
  group_exit / group_yield / send_event / throw_event /
  receive_event / tryreceive_event / switch_system_module +
  PSL1GHT-extracted spu_call_event_va_arg)
- All byte-equivalent to reference

In samples (working end-to-end in RPCS3):
- `samples/spurs/hello-spurs-getters/`  -- exercises all 11 module helpers
- `samples/lv2/hello-event-port-spu/`   -- exercises 4 event-port + yield
- `samples/lv2/hello-event-flag-spu/`   -- exercises 2 event-flag set wrappers
- `samples/spurs/hello-spurs-task/`     -- task lifecycle (yield / exit / get*)
- `samples/spurs/hello-spu-job/`        -- job-chain end-to-end
- `samples/spurs/hello-spurs-jq/`       -- job-queue end-to-end

## Reference SDK landmarks

Pre-extracted SDK 4.75 objects (pre-disassembled) live at
`/tmp/libspurs_ref/` after `spu-elf-ar x ~/cell-sdk/475.001/cell/target/spu/lib/libspurs.a`.
Re-extract any time:

```bash
mkdir -p /tmp/libspurs_ref && cd /tmp/libspurs_ref && \
    spu-elf-ar x ~/cell-sdk/475.001/cell/target/spu/lib/libspurs.a
```

Headers (read-only oracle):
- `~/cell-sdk/475.001/cell/target/spu/include/cell/spurs/semaphore.h`
- `~/cell-sdk/475.001/cell/target/spu/include/cell/spurs/barrier.h`
- `~/cell-sdk/475.001/cell/target/spu/include/cell/spurs/queue.h`
- `~/cell-sdk/475.001/cell/target/spu/include/cell/spurs/event_flag.h`
- `~/cell-sdk/475.001/cell/target/spu/include/cell/spurs/lfqueue.h`
- `~/cell-sdk/475.001/cell/target/spu/include/cell/spurs/common.h`

Per the reference policy these are CONSULTABLE for ABI / signatures /
behavior, NEVER copyable into our tree.

## Suggested attack order

1. **`cellSpursSemaphoreP` + `cellSpursSemaphoreV`** (smallest pair,
   highest-utility, demonstrates the atomic-DMA dispatch pattern).
   - Implement transitive deps first:
     `_cellSpursTaskCanCallBlockWait` (152 B, zero rels)
     `_cellSpursGetWorkloadFlag`     (48 B,  1 rodata rel)
     `_cellSpursSendWorkloadSignal`  (168 B, zero rels)
     `cellSpursSendSignal`           (464 B, calls SendWorkloadSignal)
   - Then `semaphoreP` (480 B) + `semaphoreV` (432 B).
   - Sample: 2-task taskset, producer V's, consumer P's.

2. **`cellSpursBarrierWait` + `cellSpursBarrierNotify`** (same dep
   chain as semaphore, but with notify-broadcast semantics).
   - Sample: 4-task taskset, all hit barrier, verify all proceed.

3. **`cellSpursQueuePushBegin/End` + `PopBegin/End`** (queue surface
   has a 4-call init/commit pattern; trickier).
   - Plus `queueInitialize` and the various `Get*`/`Clear`/`Depth`
     helpers (most are simple LS loads).

4. **`cellSpursEventFlagWait`** (1400 bytes, biggest single).
   - Pair with the existing event-flag set/clear primitives.
   - Mode handling: OR vs AND wait, AUTO vs MANUAL clear, direction.

5. **`cellSpursLFQueuePush/Pop`** (lock-free variant; uses different
   kernel service IDs than regular queue).

## What I learned in the survey

### Two LS dispatch tables in play

```
LS 0x1e0  module table   +0  exit fn  +4  poll fn
LS 0x2fb0 task control   +8  dispatch_base (LS pointer to per-task table)

Per-task table at *(LS 0x2fb8):
  +0x90  ElfAddress (uint64_t)
  +0xb8  TasksetAddress (uint64_t)
  +0xc4  service-call fn ptr  (cellSpursYield, BLOCK_*, etc.)
  +0xd4  TaskId (uint32_t)
```

For sync primitives the BLOCK path is:
```
il    $3, <service_id>          ; service ID (semaphore_block, etc.)
... arg setup at LS 0x2fd0 ...
lqd   $X, 0( $dispatch_base + 0xc4 )
rotqby $Y, $X, $dispatch_base+0xc4
bisl  $0, $Y                    ; CALL the kernel-side BLOCK handler
```

Kernel resumes us when the wait condition is satisfied.

### Service IDs (from disasm)

| Service ID | Used by | Notes |
|---:|---|---|
| 0 | `cellSpursTaskExit` | Already shipped |
| 1 | `cellSpursYield` / `cellSpursModulePoll` | Already shipped |
| 2 | semaphore P (BLOCK) | New, this session |
| 3 | `cellSpursTaskPoll` | Already shipped |
| ?  | barrier wait (BLOCK)  | TBD - check reference disasm |
| ?  | queue push/pop (BLOCK) | TBD |
| ?  | event-flag wait (BLOCK) | TBD |

Service ID for each new primitive needs to be confirmed by reading
the `il $3, N` immediate just before the dispatch `bisl` in each
reference function.

### Atomic DMA pattern

All sync primitives that operate on a counter/flag in main memory
use this pattern:

```
; setup MFC channels for GETLLAR (cmd 0xd0) at LS buffer
wrch $ch16, lsa          ; LSA = LS scratch buffer (0x80 in seamP)
wrch $ch17, ea_high
wrch $ch18, ea_low
wrch $ch19, size (= 0x80, full quadword)
wrch $ch20, 0            ; tag
wrch $ch21, 0xd0         ; GETLLAR cmd
rdch ch27                ; wait for tag completion
dsync

; modify the counter at LS+0x80 in scratch buffer
... atomic op ...

; setup PUTLLC (cmd 0xb4) - try-commit
wrch $ch16, lsa
... same EA ...
wrch $ch21, 0xb4         ; PUTLLC cmd
rdch ch27                ; wait
brnz <retry>             ; if conflict, restart from GETLLAR
```

### CRITICAL Makefile flag

`-fno-schedule-insns2` is REQUIRED in CFLAGS for libspurs_task.
Without it, SPU GCC's post-RA scheduler over-pads indirect calls
(`bisl`) by ~32 NOPs per call, ballooning bodies 4-5x.  The
reference SDK was clearly built with this flag; we already added it
in commit 3cf6156.  Don't remove it.

### Byte-comparing against the reference

```bash
# Per-symbol byte cmp:
spu-elf-objcopy -j .text -O binary <ours.o> /tmp/ours.bin
spu-elf-objcopy -j .text -O binary /tmp/libspurs_ref/<refname>.o /tmp/ref.bin

# Find symbol offset + size in our build:
spu-elf-objdump -t <ours.o> | grep " <symname>"

# Compare:
cmp -l \
    <(dd if=/tmp/ours.bin bs=1 skip=$((0x<offset>)) count=$((0x<size>)) 2>/dev/null) \
    <(dd if=/tmp/ref.bin bs=1 count=$((0x<size>)) 2>/dev/null)
```

Empty output = byte-identical.  Differences print as
`<index_decimal> <ours_octal> <ref_octal>`.

Common byte-mismatches to watch for:
- `nop $127` ≠ `nop $0` -- SPU GAS folds `nop $127` to `nop $0`;
  use `.long 0x4020007f` to byte-match the reference.
- `hbrr` immediate -- the encoded "branch instruction address" must
  point at the exact branch instruction, not the line before; place
  the label on the branch line.
- Trailing alignment NOPs / lnops -- objects are 16-byte aligned;
  if our function body ends with stqa/etc. the assembler may pad
  to alignment.  Often harmless.

## Files to read first

In our tree:
- `sdk/libspurs_task/src/spurs_module_runtime.c`  -- shipped helpers
  (LS 0x1c0/0x1d0/0x1e0/0x2fb0 layout reference; dispatch_call
  pattern via `_module_poll_fn()` etc.)
- `sdk/libspurs_task/src/spurs_task_runtime.c`     -- shipped
  cellSpursYield + GetTaskId + GetTasksetAddress + TaskPoll;
  reuses the per-task dispatch_base + 0xc4 fn pointer
- `sdk/libspurs_task/src/spurs_task_exit.S`        -- the exit
  syscalls; channel handshake reference (ch22/23/24)
- `sdk/libsputhread/src/event_port.S`              -- byte-exact
  hand-translation example; mailbox + stop-trap pattern
- `sdk/include/cell/spurs/common.h`                -- header for
  the round-1 + round-2 helpers; new sync primitives go here too
  (or a new sub-header)
- `cmake/ps3-self.cmake`                           -- ps3_add_spu_image
  macro; LIBS sputhread / spurs_task etc.

In memory at `/home/firebirdta01/.claude/projects/.../memory/`:
- `project_libspurs_module_getters.md`     -- round-1 background
- `project_libspurs_dispatch_helpers.md`   -- round-2 background +
  the deferred-list this prompt resolves
- `project_libsputhread_event_port.md`     -- byte-match playbook
- `project_jc_regression.md`               -- JM2 dispatcher LS layout
  + how the SPU image gets validated (BIN2 magic at LS 0x20)
- `project_spurs_job_runtime.md`           -- job-chain wire protocol
- `project_libsputhread.md`                -- earlier libsputhread
  notes; channel ch22/23/24/27/28/29/30 protocol summary

## Verification gates

For each primitive shipped:

1. **Byte-cmp against reference** -- per-symbol slice via objcopy,
   `cmp -l` should produce empty output (or only trailing-padding
   diffs).  Failures here mean the .S has wrong instruction
   ordering or register allocation.

2. **Functional test in RPCS3** -- a sample that drives the primitive
   through its happy path (semaphore: P-after-V wakes; barrier: all
   threads pass through; queue: in-order push/pop) and prints SUCCESS.

3. **Existing samples re-pass** -- after each rebuild of
   libspurs_task.a:
   - `samples/spurs/hello-spurs-getters/`  (RPCS3 -> SUCCESS)
   - `samples/spurs/hello-spurs-task/`     (RPCS3 -> SUCCESS)
   - `samples/spurs/hello-spu-job/`        (RPCS3 -> SUCCESS)
   - `samples/spurs/hello-spurs-jq/`       (RPCS3 -> SUCCESS)
   - `samples/lv2/hello-event-port-spu/`   (RPCS3 -> SUCCESS)
   - `samples/lv2/hello-event-flag-spu/`   (RPCS3 -> "Master is exiting.")

## Practical session shape

Per primitive family:

1. Extract reference object, disasm full, identify:
   - Function entry stack frame size + register saves
   - Validation pre-checks (alignment, init flag, taskset match)
   - Atomic GETLLAR/PUTLLC loop body (where + how counter is modified)
   - BLOCK path (service ID, arg setup, dispatch_call invocation)
   - Wake / unblock path (state restore, return value)
   - Exit epilogue + register restores
2. Hand-translate to .S (or .c if the C compiler emits a close-enough
   shape with `-fno-schedule-insns2`).
3. Add to libspurs_task's Makefile + install header decl in common.h.
4. Build, byte-cmp per-function, fix `nop`/`hbrr` placement issues.
5. Write or extend a sample that exercises the new primitive.
6. RPCS3 test, iterate on real-world divergence (off-by-one,
   alignment-check return code, wrong service ID, etc.) until SUCCESS.
7. Commit, update memory.

## Known dispatcher gotchas

- The kernel's BLOCK service handler may DESTROY $80..$127 across
  the call -- we've seen this in earlier work.  If a primitive's
  contract says "callee-saved across call", it must save/restore
  manually around the dispatch.
- Some BLOCK paths require the **entire** semaphore/barrier struct
  to be DMA'd from main memory before the dispatch call (kernel
  reads our LS scratch buffer to know what to wait for).
- The reference compiler emits `dsync` between MFC channel writes
  and the rdch ch27 (tag-status) read.  Don't omit it -- some
  RPCS3 builds tolerate it, real PS3 hardware doesn't.

## House rules to honour

(See memory entries `feedback_*`.)

- No vendor names ("Sony", "SCE") in tracked content -- say
  "reference SDK" or describe the binary contract directly.
- No copying reference SDK source / binary content into the repo.
  The disassembly is consulted, our implementation is hand-written.
- Don't commit without explicit user confirmation; stage and ask.
- New tracked ABI docs describe the contract directly, no
  archaeology / RE narrative -- those go to memory or untracked
  docs/ files like this one.

## Last commit to anchor against

```
3cf6156 sdk: cellSpursModuleExit/Poll/PollStatus + Poll + GetElfAddress + GetIWLTaskId  ← anchor
3a53713 sdk: libsputhread sys_spu_thread event-port + yield + switch syscalls
d30cd42 sdk: cellSpursGet{SpursAddress,CurrentSpuId,TagId,WorkloadId,SpuCount}
b87617e sdk: fix hello-spu-job halt; emit "bin2" magic at LS 0x20
a393fd9 samples + sdk: timeout-exit polish, --start-group only for multi-lib
```
