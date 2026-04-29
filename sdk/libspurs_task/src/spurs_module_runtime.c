/* libspurs SPU module runtime - thin LS-read getters + dispatch-call
 * helpers that policy modules + tasks use to interact with the SPRX
 * SPURS kernel.
 *
 * The SPU SPURS kernel keeps three control regions in well-known LS
 * locations.  Tasks run on top of these:
 *
 *   LS 0x170 (16 bytes - "spu count block")
 *     +0x06: SpuCount (uint8_t; total SPUs in this Spurs instance)
 *
 *   LS 0x1c0 (16 bytes - "module info A")
 *     +0x00..0x07: SpursAddress (uint64_t EA of the PPU CellSpurs)
 *     +0x08..0x0b: CurrentSpuId (uint32_t; 0..numSpus-1)
 *     +0x0c..0x0f: TagId (uint32_t DMA tag for this workload)
 *
 *   LS 0x1d0 (16 bytes - "module info B")
 *     +0x0c..0x0f: WorkloadId (uint32_t)
 *
 *   LS 0x1e0 (16 bytes - "module dispatch table")
 *     +0x00..0x03: exit handler fn ptr (cellSpursModuleExit)
 *     +0x04..0x07: poll handler fn ptr (cellSpursModulePoll +
 *                                       cellSpursModulePollStatus)
 *
 *   LS 0x2fb0 (16 bytes - "task control")
 *     +0x08..0x0b: dispatch_base (LS pointer; per-task service table)
 *
 *   Per-task table at *(LS 0x2fb8) [= dispatch_base]:
 *     +0x90..0x97: ElfAddress (uint64_t EA of the loaded task ELF)
 *     +0xb8..0xbf: TasksetAddress (uint64_t EA of the CellSpursTaskset)
 *     +0xc4..0xc7: service-call fn ptr (yield/poll/...)
 *     +0xd4..0xd7: TaskId (uint32_t within the taskset)
 *
 * All these blocks are filled in by the SPRX kernel before each
 * workload / task starts.  Reading them is just an LS load; calling
 * a service is `(*((fn_t)(*(uint32_t *)(base+offset)))) (arg)`.
 *
 * The reference libspurs.a emits one-or-two-instruction bodies for
 * each getter (lqa + optional rotqbyi + bi $0); GCC's plain C with a
 * volatile cast at a 16-byte-aligned LS address generates the same
 * shape.
 */
#include <stdint.h>
#include <stdbool.h>

#include <cell/spurs/types.h>

#define SPURS_SPU_COUNT_LS  0x170U   /* SpuCount at +0x6 */
#define SPURS_MOD_INFO_A    0x1c0U   /* SpursAddress / CurrentSpuId / TagId */
#define SPURS_MOD_INFO_B    0x1d0U   /* WorkloadId at +0xc */
#define SPURS_MODULE_TABLE  0x1e0U   /* exit fn @+0; poll fn @+4 */
#define SPURS_TASK_CTRL     0x2fb0U  /* dispatch_base @+8 */

/* Per-task dispatch_base offsets. */
#define SPURS_TASK_ELFADDR  0x90U
#define SPURS_TASK_TASKID   0xd4U

/* Helpers - explicit 8/4/1-byte LS reads.  uintptr_t cast first so
 * GCC sees a constant address, not a pointer-to-LS-zero. */
static inline uint64_t _ls_read64(uint32_t addr) {
    return *(const volatile uint64_t *)(uintptr_t)addr;
}
static inline uint32_t _ls_read32(uint32_t addr) {
    return *(const volatile uint32_t *)(uintptr_t)addr;
}
static inline uint8_t _ls_read8(uint32_t addr) {
    return *(const volatile uint8_t *)(uintptr_t)addr;
}

static inline uint32_t _spurs_dispatch_base(void) {
    return _ls_read32(SPURS_TASK_CTRL + 8);
}

/* Module dispatch fn pointers (cached in the per-module LS table). */
static inline void (*_module_exit_fn(void))(void) {
    return (void (*)(void))(uintptr_t)_ls_read32(SPURS_MODULE_TABLE + 0x0);
}
static inline uint32_t (*_module_poll_fn(void))(int arg) {
    return (uint32_t (*)(int))(uintptr_t)_ls_read32(SPURS_MODULE_TABLE + 0x4);
}

uint64_t cellSpursGetSpursAddress(void)
{
    return _ls_read64(SPURS_MOD_INFO_A + 0x0);
}

uint32_t cellSpursGetCurrentSpuId(void)
{
    return _ls_read32(SPURS_MOD_INFO_A + 0x8);
}

uint32_t cellSpursGetTagId(void)
{
    return _ls_read32(SPURS_MOD_INFO_A + 0xc);
}

CellSpursWorkloadId cellSpursGetWorkloadId(void)
{
    return _ls_read32(SPURS_MOD_INFO_B + 0xc);
}

uint32_t cellSpursGetSpuCount(void)
{
    /* Reference reads byte 0x176 and zero-extends to uint32_t. */
    return (uint32_t)_ls_read8(SPURS_SPU_COUNT_LS + 0x6);
}

/* The SPRX-side ELF for the running task lives in main memory; the
 * kernel stashes its EA at dispatch_base+0x90.  Returning uint64_t. */
uint64_t cellSpursGetElfAddress(void)
{
    return *(const volatile uint64_t *)(uintptr_t)
        (_spurs_dispatch_base() + SPURS_TASK_ELFADDR);
}

/* Packed (workloadId<<8 | taskId) identifier - the IWL form used by
 * trace + workload-id-aware code paths.  Reference takes the low 8
 * bits of taskId from dispatch_base+0xd4 and ORs in WorkloadId<<8. */
uint32_t _cellSpursGetIWLTaskId(void)
{
    uint32_t taskId     = *(const volatile uint32_t *)(uintptr_t)
        (_spurs_dispatch_base() + SPURS_TASK_TASKID);
    uint32_t workloadId = _ls_read32(SPURS_MOD_INFO_B + 0xc);
    return (workloadId << 8) | (taskId & 0xff);
}

/* Module-level cooperative-yield / status helpers.  The SPRX places
 * fn-ptr trampolines at LS 0x1e0; calling them re-enters the kernel.
 * These do NOT use the per-task dispatch_base at LS 0x2fb8 - that's
 * task-local; the module table is module-local. */

void cellSpursModuleExit(void)
{
    /* Reference: bisl through *(LS 0x1e0).  Doesn't return - the
     * kernel tears the module down. */
    _module_exit_fn()();
    __builtin_unreachable();
}

/* cellSpursModulePoll - ask the kernel "is there higher-priority work
 * pending?".  Reference passes service-id 1 to the poll fn, then
 * compares the response to WorkloadId at LS 0x1dc; returns non-zero
 * if the workload was preempted (i.e. the SPU should yield). */
int cellSpursModulePoll(void)
{
    uint32_t myWorkload = _ls_read32(SPURS_MOD_INFO_B + 0xc);
    uint32_t pollResult = _module_poll_fn()(1);
    /* 0 if pollResult == myWorkload (no preemption); non-zero else. */
    return pollResult != myWorkload;
}

/* cellSpursModulePollStatus - same poll, but writes the kernel's
 * raw response (the workloadId of the highest-priority pending
 * workload, if any) into *pStatus when non-NULL.  Returns the same
 * "should-yield" boolean as cellSpursModulePoll. */
int cellSpursModulePollStatus(uint32_t *pStatus)
{
    uint32_t myWorkload = _ls_read32(SPURS_MOD_INFO_B + 0xc);
    uint32_t pollResult = _module_poll_fn()(1);
    if (pStatus) *pStatus = pollResult;
    return pollResult != myWorkload;
}

/* cellSpursPoll - module-level convenience: returns true iff the
 * SPU should yield to higher-priority work.  Same shape as
 * cellSpursModulePoll; reference exposes both names. */
bool cellSpursPoll(void)
{
    return cellSpursModulePoll() != 0;
}
