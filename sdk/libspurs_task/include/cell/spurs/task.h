/* cell/spurs/task.h - SPU-side Spurs task runtime surface.
 *
 * Canonical include for SPU ELFs that ship as Spurs tasks.  Declares
 * the canonical task entry (cellSpursTaskMain), the legacy entry the CRT
 * calls (cellSpursMain), and the task-side SPURS API.  Including this
 * header also makes the SPURS error codes (and CELL_OK), the shared
 * SPURS types, the task/taskset types and the common SPU context getters
 * (cellSpursGetCurrentSpuId and friends) visible.
 *
 * Build expectations:
 *   spu-elf-gcc -nostartfiles -T $(PS3DK)/spu/ldscripts/spurs_task.ld
 *               -lspurs_task
 *
 * The user provides:
 *   int cellSpursTaskMain(qword argTask, uint64_t argTaskset);
 * Its result is passed to cellSpursTaskExit. A user-provided legacy
 * cellSpursMain overrides the archive bridge, including when both entries
 * are defined. Link application objects before libspurs_task.a.
 *
 * argTask arrives in r3 (full 16-byte vector),
 * argTaskset arrives in r4 preferred slot (64-bit EA).
 *
 * Declared-only entry points: some functions below are part of the SPU
 * task API but are not yet implemented by libspurs_task.a (for example
 * cellSpursWaitSignal, cellSpursShutdownTaskset, cellSpursCreateTask*,
 * cellSpursJoinTask2 and the LS-pattern queries).  Code that calls them
 * compiles and fails at link time until the runtime grows them.
 */
#ifndef __PS3DK_CELL_SPURS_TASK_H__
#define __PS3DK_CELL_SPURS_TASK_H__

#ifndef __SPU__
#error "cell/spurs/task.h is SPU-only"
#endif

#include <stdint.h>
#include <spu_intrinsics.h>

#include <cell/spurs/types.h>
#include <cell/spurs/task_types.h>
#include <cell/spurs/error.h>
#include <cell/spurs/version.h>
#include <cell/spurs/common.h>

/* Return values of cellSpursTaskPoll / cellSpursTaskPoll2 (bit set). */
#define CELL_SPURS_TASK_POLL_FOUND_TASK       1
#define CELL_SPURS_TASK_POLL_FOUND_WORKLOAD   2

/* Revision passed by the cellSpursTaskAttributeInitialize wrapper. */
#define _CELL_SPURS_TASK_ATTRIBUTE_REVISION   0x01

/* LS pattern covering every 2 KB block a task may use (everything from
 * CELL_SPURS_TASK_TOP to the end of local storage). */
#define CELL_SPURS_TASK_LS_ALL \
    ((vec_uint4){ CELL_SPURS_TASK_TOP_MASK, 0xffffffffu, 0xffffffffu, 0xffffffffu })

/* Where and how much of a task's context to save when it is switched
 * out: the EA of the save area, its size, and the LS pattern to save. */
typedef struct CellSpursTaskSaveConfig {
    uint64_t  eaContext;
    uint32_t  sizeContext;
    vec_uint4 lsPattern;
} CellSpursTaskSaveConfig;

/* CELL_SPURS_TASK_LS_ALL in CellSpursTaskLsPattern form. */
static const CellSpursTaskLsPattern gCellSpursTaskLsAll = {
    { CELL_SPURS_TASK_TOP_MASK, 0xffffffffu, 0xffffffffu, 0xffffffffu }
};

#ifdef __cplusplus
extern "C" {
#endif

/* -- Entry points ---------------------------------------------------- */

/* Canonical entry: return the task exit code. Both entries have C linkage. */
int cellSpursTaskMain(qword argTask, uint64_t argTaskset);
/* Legacy override; crt branches here on task start. */
void cellSpursMain(qword argTask, uint64_t argTaskset);

/* -- Running-task control (libspurs_task.a, via the 0x2fb0 block) ---- */

void            cellSpursExit(void)           __attribute__((noreturn));
void            cellSpursTaskExit(int code)   __attribute__((noreturn));
CellSpursTaskId cellSpursGetTaskId(void);
uint64_t        cellSpursGetTasksetAddress(void);
int             cellSpursYield(void);
unsigned        cellSpursTaskPoll(void);

/* Declared only (not yet in libspurs_task.a). */
int             cellSpursYield2(void);
unsigned        cellSpursTaskPoll2(void);
int             cellSpursWaitSignal(void);
int             cellSpursWaitSignal2(void);
int             cellSpursGetTaskVolatileArea(void **ptr, uint32_t *size);
int             cellSpursTaskReceiveWorkloadFlag(void);
int             cellSpursTaskReceiveWorkloadFlag2(void);

/* -- Taskset / task control from SPU --------------------------------- */

/* Wake task `idTask` of the taskset at `eaTaskset` (libspurs_task.a). */
int cellSpursSendSignal(uint64_t eaTaskset, CellSpursTaskId idTask);

/* Declared only (not yet in libspurs_task.a). */
int cellSpursShutdownTaskset(uint64_t eaTaskset);

int cellSpursCreateTask(uint64_t eaTaskset, CellSpursTaskId *idTask,
                        uint64_t eaElf, uint64_t eaContext,
                        uint32_t sizeContext, vec_uint4 lsPattern,
                        qword argument);
int cellSpursCreateTaskWithAttribute(uint64_t eaTaskset,
                                     CellSpursTaskId *idTask,
                                     const CellSpursTaskAttribute *attr);
int cellSpursCreateTask2(uint64_t eaTaskset, CellSpursTaskId *idTask,
                         uint64_t eaElf, qword argument,
                         const CellSpursTaskAttribute2 *attr);
int cellSpursCreateTask2WithBinInfo(uint64_t eaTaskset,
                                    CellSpursTaskId *idTask,
                                    uint64_t eaTaskBinInfo, qword argument,
                                    uint64_t eaContext, const char *name,
                                    void *__reserved__);
int cellSpursJoinTask2(uint64_t eaTaskset, CellSpursTaskId idTask,
                       int *exitCode);
int cellSpursTryJoinTask2(uint64_t eaTaskset, CellSpursTaskId idTask,
                          int *exitCode);

int _cellSpursTaskAttributeInitialize(CellSpursTaskAttribute *attr,
                                      unsigned int revision,
                                      unsigned int sdkVersion,
                                      uint64_t eaElf,
                                      const CellSpursTaskSaveConfig *saveConfig,
                                      qword argument);
int cellSpursTaskAttributeSetExitCodeContainer(CellSpursTaskAttribute *attr,
                                               uint64_t eaExitCode);
void _cellSpursTaskAttribute2Initialize(CellSpursTaskAttribute2 *attr,
                                        uint32_t revision);

/* -- LS context pattern ---------------------------------------------- */

/* Declared only (not yet in libspurs_task.a). */
vec_uint4 cellSpursContextGetLsPattern(void);
int       cellSpursContextSetLsPattern(vec_uint4 lsPattern);
int       cellSpursTaskGenerateLsPattern(vec_uint4 *lsPattern,
                                         uint32_t start, uint32_t size);
int       cellSpursTaskGetReadOnlyAreaPattern(vec_uint4 *lsPattern,
                                              uint64_t eaElf, void *buf,
                                              uint32_t tag);
int       cellSpursTaskGetLoadableSegmentPattern(vec_uint4 *lsPattern,
                                                 uint64_t eaElf, void *buf,
                                                 uint32_t tag);
int       cellSpursTaskGetContextSaveAreaSize(uint32_t *size,
                                              vec_uint4 lsPattern);

#ifdef __cplusplus
}   /* extern "C" */
#endif

/* -- Inline helpers -------------------------------------------------- */

static inline int
cellSpursTaskAttributeInitialize(CellSpursTaskAttribute *attr,
                                 uint64_t eaElf,
                                 const CellSpursTaskSaveConfig *saveConfig,
                                 qword argument)
{
    return _cellSpursTaskAttributeInitialize(attr,
                                             _CELL_SPURS_TASK_ATTRIBUTE_REVISION,
                                             _CELL_SPURS_INTERNAL_VERSION,
                                             eaElf, saveConfig, argument);
}

static inline void
cellSpursTaskAttribute2Initialize(CellSpursTaskAttribute2 *attr)
{
    _cellSpursTaskAttribute2Initialize(attr, CELL_SPURS_TASK2_REVISION);
}

/* Build the LS pattern that covers [start, start + size): one bit per
 * 2 KB block of local storage, block 0 in the most significant bit of
 * word 0.  The end is rounded up to the next 2 KB boundary. */
static inline vec_uint4
cellSpursContextGenerateLsPattern(intptr_t start, int size)
{
    const int first = (int)CELL_SPURS_CONTEXT_SIZE2BITS(start);
    const int end   = (int)CELL_SPURS_CONTEXT_SIZE2BITS(start + size + 2047);
    unsigned int word[4];
    int k;

    for (k = 0; k < 4; ++k) {
        int lo = first - 32 * k;
        int hi = end - 32 * k;
        if (lo < 0)  lo = 0;
        if (hi > 32) hi = 32;
        if (lo >= hi) {
            word[k] = 0;
        } else {
            unsigned int from_lo  = 0xffffffffu >> lo;
            unsigned int below_hi = (hi == 32) ? 0u : (0xffffffffu >> hi);
            word[k] = from_lo & ~below_hi;
        }
    }
    return (vec_uint4){ word[0], word[1], word[2], word[3] };
}

#ifdef __cplusplus
/* The C++ task-side wrappers live next to their primitives. */
#include <cell/spurs/event_flag.h>
#include <cell/spurs/barrier.h>
#include <cell/spurs/semaphore.h>
#include <cell/spurs/queue.h>
#include <cell/spurs/lfqueue.h>
#endif

#endif /* __PS3DK_CELL_SPURS_TASK_H__ */
