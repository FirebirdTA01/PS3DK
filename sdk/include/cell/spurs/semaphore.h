/* cell/spurs/semaphore.h - SPURS counting semaphore.
 *
 * CellSpursSemaphore is a 128-byte opaque block; `total` is the
 * initial count.  Semantics match classic P/V - consumer side
 * blocks when count==0; producer side signals to increment.
 * The counting-side operations live on the SPU and aren't exposed
 * from PPU; PPU drives Initialize and GetTasksetAddress.
 */
#ifndef __PS3DK_CELL_SPURS_SEMAPHORE_H__
#define __PS3DK_CELL_SPURS_SEMAPHORE_H__

#include <cell/spurs/types.h>

#ifdef __cplusplus
extern "C" {
#endif

#define CELL_SPURS_SEMAPHORE_ALIGN  128
#define CELL_SPURS_SEMAPHORE_SIZE   128

typedef struct CellSpursSemaphore {
    unsigned char skip[CELL_SPURS_SEMAPHORE_SIZE];
} __attribute__((aligned(CELL_SPURS_SEMAPHORE_ALIGN))) CellSpursSemaphore;

extern int _cellSpursSemaphoreInitialize(CellSpurs *spurs,
                                         CellSpursTaskset *taskset,
                                         CellSpursSemaphore *semaphore,
                                         int total);

extern int cellSpursSemaphoreGetTasksetAddress(const CellSpursSemaphore *semaphore,
                                               CellSpursTaskset **taskset);

static inline int
cellSpursSemaphoreInitialize(CellSpursTaskset *taskset,
                             CellSpursSemaphore *semaphore,
                             int total)
{ return _cellSpursSemaphoreInitialize(0, taskset, semaphore, total); }

static inline int
cellSpursSemaphoreInitializeIWL(CellSpurs *spurs,
                                CellSpursSemaphore *semaphore,
                                int total)
{ return _cellSpursSemaphoreInitialize(spurs, 0, semaphore, total); }

#ifdef __SPU__
/* SPU-side counting operations.  These take a 64-bit EA pointing at
 * the CellSpursSemaphore in main memory and run an atomic GETLLAR /
 * PUTLLC retry loop on the cache line.  P() blocks the calling task
 * via the SPRX BLOCK service when the count is zero; V() wakes the
 * oldest waiter (if any) by calling the workload-signal path. */
extern int cellSpursSemaphoreP(uint64_t eaSemaphore);
extern int cellSpursSemaphoreV(uint64_t eaSemaphore);

/* Task-signal helper: wake a specific task in a taskset by EA + index.
 * Uses GETLLAR/PUTLLC on the taskset signal line and dispatches the
 * workload-signal path if the bit transitions 0 -> 1. */
extern int cellSpursSendSignal(uint64_t eaTaskset, int taskIndex);

/* Internal SPU runtime helpers exported for cross-object calls inside
 * libspurs_task.a.  Not part of the public surface but declared here
 * so user code that re-implements a wait primitive in assembly has a
 * symbol to brsl against. */
extern int      _cellSpursTaskCanCallBlockWait(void);
extern uint64_t _cellSpursGetWorkloadFlag(void);
extern int      _cellSpursSendWorkloadSignal(int signalBit);
#endif /* __SPU__ */

#ifdef __cplusplus
}   /* extern "C" */
#endif

#endif /* __PS3DK_CELL_SPURS_SEMAPHORE_H__ */
