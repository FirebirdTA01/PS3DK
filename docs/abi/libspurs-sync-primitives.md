# libspurs SPU sync primitives — binary contracts

Six SPU runtime symbols shipped in `libspurs_task.a` for SPU tasks
that need atomic-DMA + BLOCK-service synchronization.

## Public surface

```c
/* cell/spurs/semaphore.h, guarded by #ifdef __SPU__ */

int      cellSpursSemaphoreP(uint64_t eaSemaphore);
int      cellSpursSemaphoreV(uint64_t eaSemaphore);
int      cellSpursSendSignal(uint64_t eaTaskset, int taskIndex);

int      _cellSpursTaskCanCallBlockWait(void);
uint64_t _cellSpursGetWorkloadFlag(void);
int      _cellSpursSendWorkloadSignal(int signalBit);
```

## Calling convention

Standard Cell OS SPU ABI:
- arguments in `r3`/`r4` preferred slots
- return value in `r3` preferred slot
- callee-saved: `r80..r127`
- caller-saved: `r2..r79`
- `r0` is LR; `r1` is SP

`uint64_t` arguments occupy `r3` bytes 0..7 (big-endian).  The
internal NULL-pointer guard in `cellSpursSemaphoreP`,
`cellSpursSemaphoreV`, and `cellSpursSendSignal` examines all
sixteen bytes of `r3`: it triggers (returns `0x80410911`) only when
at least three of the four 32-bit slots are simultaneously zero.
GCC `-O2` loads u64 EA arguments from a 16-byte slot whose upper
half holds other live data, which always satisfies the guard.

## Semaphore cache-line layout (128 B in main memory)

`CellSpursSemaphore` is 128 bytes.  Alignment is 16 bytes for
sdkVersion < 0x27FFFF, 128 bytes thereafter.  The SPU operations
run a `GETLLAR` / `PUTLLC` retry loop on this line.

| Offset | Bytes | Role |
|-------:|------:|------|
|  +0x00 |   16  | counter (signed 32-bit at +0) + flags |
|  +0x10 |   16  | waiter list head (16-bit task-id list) |
|  +0x20 |   16  | waiter list info (paired with +0x10) |
|  +0x30 |   16  | bound-taskset / bound-spurs EA + flags |
|  +0x40 |   32  | kernel-side scratch |
|  +0x60 |   16  | originating-Spurs `SpursAddress` snapshot |
|  +0x70 |   16  | reserved / state bits |

`P` decrements the counter when positive.  When the counter is
zero the calling task is queued in the waiter list,
`_cellSpursTaskCanCallBlockWait` verifies the call site is in a
blocking-eligible context, and the BLOCK service is invoked
(`service-id 2` through the per-task dispatch function at
`*(LS dispatch_base + 0xc4)`).  Control returns to `P` only after
a matching `V` runs.

`V` increments the counter when no waiter is queued.  Otherwise
it pops the oldest waiter, calls `_cellSpursGetWorkloadFlag` to
obtain the signal-cache-line EA, then `cellSpursSendSignal` to
set the waiter's signal bit and dispatch the workload-signal
path.

## Error codes

All returns are `0` on success or a standard
`CELL_SPURS_TASK_ERROR_*` code from `cell/spurs/error.h`:

| Code | Trigger |
|------|---------|
| `0x80410901` | waiter slot full / queue overflow |
| `0x80410902` | bound-spurs invalid / taskset signal range |
| `0x80410909` | gate: blocking not permitted in this context |
| `0x8041090f` | gate: kernel state not consistent |
| `0x80410910` | EA not 16-byte aligned |
| `0x80410911` | NULL EA |

## Caller obligations

For `cellSpursSemaphoreP` to reach the BLOCK service successfully,
`cellSpursCreateTask` must have been called with a non-NULL
`eaContext` and a `sizeContext` of at least
`CELL_SPURS_TASK_EXECUTION_CONTEXT_SIZE`.  Tasks created with
`eaContext == NULL` may only call non-blocking entry points
(`cellSpursTaskExit`, `cellSpursYield`, `cellSpursTaskPoll`); the
gate returns `0x8041090f` if the context save area is missing.

The `lsPattern` argument to `cellSpursCreateTask` must mask out
chunks below `CELL_SPURS_TASK_TOP` (LS `0x3000`).  The header
provides `CELL_SPURS_TASK_TOP_MASK` for the first u32; the
remaining three u32s are typically `0xffffffff`.

## Compiler flag requirement

`libspurs_task` must be built with `-fno-schedule-insns2`.  SPU
GCC's post-RA scheduler over-pads indirect calls (`bisl`) by
~32 NOPs per call, which alters the byte sizes of every routine
that uses the dispatch path, including all six symbols above and
the existing `cellSpursExit` / `cellSpursTaskExit`.  The flag is
set in `sdk/libspurs_task/Makefile`.
