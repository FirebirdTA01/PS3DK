# libfiber (cellFiber) — PPU fiber scheduling

| Property | Value |
|---|---|
| Header umbrella | `<cell/fiber.h>` |
| Stub archive | `libfiber_stub.a` (nidgen) |
| SPRX module | `CELL_SYSMODULE_FIBER` (0x0043) |
| Entry points | 47 SPRX exports + 1 public wrapper = 48 |
| Internal version | `_CELL_FIBER_PPU_INTERNAL_VERSION` = 0x330000 |

## Surface

libfiber provides four feature areas, all PPU:

| Area | Files | Description |
|---|---|---|
| **Context** | `ppu_context.h`, `ppu_context_types.h` | Low-level co-operative fiber primitive — no scheduler, just initialize / switch / return-to-thread |
| **Fiber** | `ppu_fiber.h`, `ppu_fiber_types.h` | Scheduled fibers on top of `CellFiberPpuScheduler` with priority queues, wait-flag, send-signal, yield, join |
| **Worker control** | `ppu_fiber_worker_control.h` | Wraps a scheduler with a pool of worker PPU threads; SPU-callable subset via effective-address handles |
| **Trace** | `ppu_fiber_trace_types.h`, `user_scheduler.h` | Scheduler-level trace ring buffer + user scheduler callbacks |

### Function table

| Function | Signature |
|---|---|
| `cellFiberPpuInitialize` | `int cellFiberPpuInitialize(void)` — public wrapper, loads SPRX first |
| `_cellFiberPpuInitialize` | `int _cellFiberPpuInitialize(void *tlsArea)` — SPRX internal init |

**Context API** (7 public + 1 underscored + 1 inline):

| Function | Signature |
|---|---|
| `_cellFiberPpuContextAttributeInitialize` | `int(CellFiberPpuContextAttribute*, uint32_t sdkVersion)` |
| `cellFiberPpuContextAttributeInitialize` | inline → underscored |
| `cellFiberPpuContextInitialize` | `int(CellFiberPpuContext*, CellFiberPpuContextEntry, uint64_t arg, void *eaStack, size_t stackSize, const CellFiberPpuContextAttribute*)` |
| `cellFiberPpuContextFinalize` | `int(CellFiberPpuContext*)` |
| `cellFiberPpuContextRun` | `int(CellFiberPpuContext*, int *cause, CellFiberPpuContext**, const CellFiberPpuContextExecutionOption*)` |
| `cellFiberPpuContextSwitch` | `int(CellFiberPpuContext*, CellFiberPpuContext**, const CellFiberPpuContextExecutionOption*)` |
| `cellFiberPpuContextReturnToThread` | `int(int cause)` |
| `cellFiberPpuContextSelf` | `CellFiberPpuContext*(void)` |
| `cellFiberPpuContextCheckStackLimit` | `int(void)` |

**Scheduler API** (6 public + 1 underscored + 1 inline):

| Function | Signature |
|---|---|
| `_cellFiberPpuSchedulerAttributeInitialize` | `int(CellFiberPpuSchedulerAttribute*, uint32_t)` |
| `cellFiberPpuSchedulerAttributeInitialize` | inline → underscored |
| `cellFiberPpuInitializeScheduler` | `int(CellFiberPpuScheduler*, const CellFiberPpuSchedulerAttribute*)` |
| `cellFiberPpuFinalizeScheduler` | `int(CellFiberPpuScheduler*)` |
| `cellFiberPpuRunFibers` | `int(CellFiberPpuScheduler*)` |
| `cellFiberPpuCheckFlags` | `int(CellFiberPpuScheduler*)` |
| `cellFiberPpuHasRunnableFiber` | `int(CellFiberPpuScheduler*, bool*)` |
| `cellFiberPpuGetScheduler` | `int(CellFiberPpu*, CellFiberPpuScheduler**)` |

**Fiber API** (9 public + 1 underscored + 1 inline):

| Function | Signature |
|---|---|
| `cellFiberPpuCreateFiber` | `int(CellFiberPpuScheduler*, CellFiberPpu*, CellFiberPpuEntry, uint64_t, unsigned int priority, void *eaStack, size_t, const CellFiberPpuAttribute*)` |
| `cellFiberPpuSelf` | `CellFiberPpu*(void)` |
| `cellFiberPpuWaitFlag` | `int(uint32_t *eaFlag, bool flagValue)` |
| `cellFiberPpuWaitSignal` | `int(void)` |
| `cellFiberPpuSetPriority` | `int(unsigned int)` |
| `cellFiberPpuSendSignal` | `int(CellFiberPpu*, unsigned int *numWorker)` |
| `cellFiberPpuYield` | `int(void)` |
| `cellFiberPpuExit` | `int(int exitCode)` |
| `cellFiberPpuJoinFiber` | `int(CellFiberPpu*, int *exitCode)` |
| `cellFiberPpuCheckStackLimit` | `int(void)` |

**Worker control API** — PPU side (12 public + 1 underscored + 1 inline):

All take `CellFiberPpuUtilWorkerControl*` as first argument. SPU side has 2 entry points using `uint32_t eaControl` / `uint32_t eaFiber` effective addresses.

**Trace** (4 functions):

| Function | Signature |
|---|---|
| `cellFiberPpuSchedulerTraceInitialize` | `int(CellFiberPpuScheduler*, void *buffer, size_t size, uint32_t mode)` |
| `cellFiberPpuSchedulerTraceStart` | `int(CellFiberPpuScheduler*)` |
| `cellFiberPpuSchedulerTraceStop` | `int(CellFiberPpuScheduler*)` |
| `cellFiberPpuSchedulerTraceFinalize` | `int(CellFiberPpuScheduler*)` |

**User scheduler** (2 functions):

| Function | Signature |
|---|---|
| `cellFiberPpuContextRunScheduler` | `int(CellFiberPpuSchedulerCallback, uint64_t, uint64_t, int*, CellFiberPpuContext**, const CellFiberPpuContextExecutionOption*)` |
| `cellFiberPpuContextEnterScheduler` | `int(CellFiberPpuSchedulerCallback, uint64_t, uint64_t, CellFiberPpuContext**, const CellFiberPpuContextExecutionOption*)` |

## Error codes

All errors encode in `0x8076xxxx` (facility `CELL_ERROR_FACILITY_FIBER` = 0x076):

| Error | Value |
|---|---|
| `CELL_FIBER_ERROR_AGAIN` | 0x80760001 |
| `CELL_FIBER_ERROR_INVAL` | 0x80760002 |
| `CELL_FIBER_ERROR_NOSYS` | 0x80760003 |
| `CELL_FIBER_ERROR_NOMEM` | 0x80760004 |
| `CELL_FIBER_ERROR_SRCH` | 0x80760005 |
| `CELL_FIBER_ERROR_NOENT` | 0x80760006 |
| `CELL_FIBER_ERROR_NOEXEC` | 0x80760007 |
| `CELL_FIBER_ERROR_DEADLK` | 0x80760008 |
| `CELL_FIBER_ERROR_PERM` | 0x80760009 |
| `CELL_FIBER_ERROR_BUSY` | 0x8076000A |
| `CELL_FIBER_ERROR_ABORT` | 0x8076000C |
| `CELL_FIBER_ERROR_FAULT` | 0x8076000D |
| `CELL_FIBER_ERROR_CHILD` | 0x8076000E |
| `CELL_FIBER_ERROR_STAT` | 0x8076000F |
| `CELL_FIBER_ERROR_ALIGN` | 0x80760010 |
| `CELL_FIBER_ERROR_NULL_POINTER` | 0x80760011 |
| `CELL_FIBER_ERROR_NOSYSINIT` | 0x80760020 |

## Initialisation lifecycle

1. `cellFiberPpuInitialize()` — loads `CELL_SYSMODULE_FIBER` (if not already loaded), then calls `_cellFiberPpuInitialize(&tlsArea)` with a 64-byte TLS area (`_gCellFiberPpuThreadLocalStorage`).
2. Create a scheduler: `cellFiberPpuSchedulerAttributeInitialize` → `cellFiberPpuInitializeScheduler`.
3. Create fibers: `cellFiberPpuAttributeInitialize` → `cellFiberPpuCreateFiber`.
4. Run: `cellFiberPpuRunFibers` (blocks until no runnable fibers) or use `cellFiberPpuContextRunScheduler` for a custom scheduling callback.
5. Join fibers: `cellFiberPpuJoinFiber`.
6. Tear down: `cellFiberPpuFinalizeScheduler`.

## Notable ABI quirks

### ATTRIBUTE_PRXPTR

Every pointer field in structs crossing the SPRX boundary must carry `ATTRIBUTE_PRXPTR`. This macro (from `<ppu-types.h>`) compresses pointers from LP64 8-byte storage to the 4-byte effective address the SPRX expects.

| Struct | PRXPTR fields |
|---|---|
| `CellFiberPpuAttribute` | `onExitCallback` (function pointer) — followed by `uint32_t __reserved0__` pad (4+4=8 bytes total) |
| `CellFiberPpuContext` | Internal opaque — no user-visible pointers |
| `CellFiberPpuUtilWorkerControlAttribute` | Internal opaque |

The `onExitCallback` padding pattern is critical: the function pointer takes 4 bytes (PRXPTR), a 4-byte `__reserved0__` follows, then `onExitCallbackArg` at offset 56. Without PRXPTR the function pointer would be 8 bytes and every subsequent offset would shift by 4, breaking the struct's 256-byte budget.

### Struct sizes

| Struct | Size | Align |
|---|---|---|
| `CellFiberPpuAttribute` | 256 | 8 |
| `CellFiberPpu` | 896 (640+256) | 128 |
| `CellFiberPpuSchedulerAttribute` | 256 | 8 |
| `CellFiberPpuScheduler` | 512 (128*4) | 128 |
| `CellFiberPpuContextAttribute` | 128 | 8 |
| `CellFiberPpuContext` | 640 (128*5) | 16 |
| `CellFiberPpuUtilWorkerControl` | 768 (512+256) | 128 |
| `CellFiberPpuTracePacket` | 16 | 16 |

### PPC64 ELFv1 .opd

The `cellFiberPpuInitialize` public wrapper uses a hand-written assembly prologue (`fiber_init_wrapper.S`) to export both the `.opd` function descriptor (D symbol) and the `.cellFiberPpuInitialize` text entry point (T symbol), matching the reference archive's shape.

### TLS area

`_gCellFiberPpuThreadLocalStorage[64]` — 64-byte BSS area (`nm -S` of reference shows size 0x40). The SPRX uses this for thread-local storage referenced during fiber switching.

## Sample

`samples/lv2/hello-ppu-fiber/` — PPU smoke test validated in RPCS3: creates two fibers, each yielding a few times then exiting, joins both, tears down the scheduler.
