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

#ifdef __cplusplus
}   /* extern "C" */
#endif

#endif /* __PS3DK_CELL_SPURS_SEMAPHORE_H__ */
