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

#ifdef __SPU__

/* SPU side: the semaphore lives in main memory and is named by its
 * effective address.  P() blocks the calling task (valid only in a
 * SPURS task) while the count is zero; V() increments it and wakes a
 * waiter.  P and V are in libspurs_task.a; Initialize and
 * GetTasksetAddress are declared only (not yet in the SPU runtime). */
extern int _cellSpursSemaphoreInitialize(uint64_t ea, int total,
                                         unsigned isIwl);
extern int cellSpursSemaphoreP(uint64_t ea);
extern int cellSpursSemaphoreV(uint64_t ea);
extern int cellSpursSemaphoreGetTasksetAddress(uint64_t ea,
                                               uint64_t *pEaTaskset);

#define cellSpursSemaphoreInitialize(ea, total) \
    _cellSpursSemaphoreInitialize((ea), (total), 0)
#define cellSpursSemaphoreInitializeIWL(ea, total) \
    _cellSpursSemaphoreInitialize((ea), (total), 1)

/* Task-signal helper: wake task `idTask` of the taskset at `eaTaskset`
 * (also declared by cell/spurs/task.h). */
extern int cellSpursSendSignal(uint64_t eaTaskset, CellSpursTaskId idTask);

/* Internal SPU runtime helpers exported for cross-object calls inside
 * libspurs_task.a.  Not part of the public surface but declared here
 * so user code that re-implements a wait primitive in assembly has a
 * symbol to brsl against. */
extern int      _cellSpursTaskCanCallBlockWait(void);
extern uint64_t _cellSpursGetWorkloadFlag(void);
extern int      _cellSpursSendWorkloadSignal(int signalBit);

#ifdef __cplusplus
}   /* extern "C" */

namespace cell {
namespace Spurs {

class Semaphore : public CellSpursSemaphore {
public:
    static const uint32_t kAlign = CELL_SPURS_SEMAPHORE_ALIGN;
    static const uint32_t kSize  = CELL_SPURS_SEMAPHORE_SIZE;
};

/* SPU handle on a semaphore in main memory: holds its EA and forwards
 * to the EA-based C API. */
class SemaphoreStub {
protected:
    uint64_t object_ea;

public:
    static const uint32_t kAlign = CELL_SPURS_SEMAPHORE_ALIGN;
    static const uint32_t kSize  = CELL_SPURS_SEMAPHORE_SIZE;

    void setObject(uint64_t ea) { object_ea = ea; }
    uint64_t getObject(void) const { return object_ea; }

    int initialize(int total) const
    { return cellSpursSemaphoreInitialize(object_ea, total); }
    int initializeIWL(int total) const
    { return cellSpursSemaphoreInitializeIWL(object_ea, total); }
    int p(void) const { return cellSpursSemaphoreP(object_ea); }
    int v(void) const { return cellSpursSemaphoreV(object_ea); }
    int getTasksetAddress(uint64_t *pEaTaskset) const
    { return cellSpursSemaphoreGetTasksetAddress(object_ea, pEaTaskset); }
};

}   /* namespace Spurs */
}   /* namespace cell */

#endif /* __cplusplus */

#else /* PPU */

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

namespace cell {
namespace Spurs {

class Semaphore : public CellSpursSemaphore {
public:
    static const uint32_t kAlign = CELL_SPURS_SEMAPHORE_ALIGN;
    static const uint32_t kSize  = CELL_SPURS_SEMAPHORE_SIZE;

    static int initialize(CellSpursTaskset *taskset,
                          CellSpursSemaphore *semaphore,
                          int total)
    { return cellSpursSemaphoreInitialize(taskset, semaphore, total); }

    static int initializeIWL(CellSpurs *spurs,
                             CellSpursSemaphore *semaphore,
                             int total)
    { return cellSpursSemaphoreInitializeIWL(spurs, semaphore, total); }

    int getTasksetAddress(CellSpursTaskset **taskset) const
    { return cellSpursSemaphoreGetTasksetAddress(this, taskset); }
};

}   /* namespace Spurs */
}   /* namespace cell */

#endif

#endif /* __SPU__ */

#endif /* __PS3DK_CELL_SPURS_SEMAPHORE_H__ */
