/* cell/spurs/barrier.h - SPURS task-side barrier.
 *
 * CellSpursBarrier is a 128-byte opaque block associated with a
 * taskset; tasks bound to the taskset call wait / notify on the SPU
 * side.  PPU-side surface is just Initialize + GetTasksetAddress.
 */
#ifndef __PS3DK_CELL_SPURS_BARRIER_H__
#define __PS3DK_CELL_SPURS_BARRIER_H__

#include <stdint.h>
#include <cell/spurs/types.h>
#include <cell/spurs/task_types.h>

#ifdef __cplusplus
extern "C" {
#endif

#define CELL_SPURS_BARRIER_ALIGN    128
#define CELL_SPURS_BARRIER_SIZE     128

typedef struct CellSpursBarrier {
    unsigned char skip[CELL_SPURS_BARRIER_SIZE];
} __attribute__((aligned(CELL_SPURS_BARRIER_ALIGN))) CellSpursBarrier;

#ifdef __SPU__

/* SPU side: the barrier lives in main memory and is named by its
 * effective address.  A task notifies its arrival, then waits for the
 * others; the blocking forms are valid only in a SPURS task.
 * Implemented in libspurs.a / libspurs_task.a. */
extern int cellSpursBarrierInitialize(uint64_t ea, unsigned int total);
extern int _cellSpursBarrierNotify(uint64_t ea, unsigned isBlocking);
extern int _cellSpursBarrierWait(uint64_t ea, unsigned isBlocking);
extern int cellSpursBarrierGetTasksetAddress(uint64_t ea,
                                             uint64_t *pEaTaskset);

#define cellSpursBarrierNotify(ea)     _cellSpursBarrierNotify((ea), 1)
#define cellSpursBarrierTryNotify(ea)  _cellSpursBarrierNotify((ea), 0)
#define cellSpursBarrierWait(ea)       _cellSpursBarrierWait((ea), 1)
#define cellSpursBarrierTryWait(ea)    _cellSpursBarrierWait((ea), 0)

#ifdef __cplusplus
}   /* extern "C" */

namespace cell {
namespace Spurs {

class Barrier : public CellSpursBarrier {
public:
    static const uint32_t kAlign = CELL_SPURS_BARRIER_ALIGN;
    static const uint32_t kSize  = CELL_SPURS_BARRIER_SIZE;
};

/* SPU handle on a barrier in main memory: holds its EA and forwards to
 * the EA-based C API. */
class BarrierStub {
protected:
    uint64_t object_ea;

public:
    static const uint32_t kAlign = CELL_SPURS_BARRIER_ALIGN;
    static const uint32_t kSize  = CELL_SPURS_BARRIER_SIZE;

    void setObject(uint64_t ea) { object_ea = ea; }
    uint64_t getObject(void) const { return object_ea; }

    int initialize(unsigned int total) const
    { return cellSpursBarrierInitialize(object_ea, total); }
    int getTasksetAddress(uint64_t *pEaTaskset) const
    { return cellSpursBarrierGetTasksetAddress(object_ea, pEaTaskset); }
    int tryNotify(void) const { return _cellSpursBarrierNotify(object_ea, 0); }
    int notify(void) const    { return _cellSpursBarrierNotify(object_ea, 1); }
    int tryWait(void) const   { return _cellSpursBarrierWait(object_ea, 0); }
    int wait(void) const      { return _cellSpursBarrierWait(object_ea, 1); }
};

}   /* namespace Spurs */
}   /* namespace cell */

#endif   /* __cplusplus */

#else /* PPU */

extern int cellSpursBarrierInitialize(CellSpursTaskset *taskset,
                                      CellSpursBarrier *barrier,
                                      unsigned int total);
extern int cellSpursBarrierGetTasksetAddress(const CellSpursBarrier *barrier,
                                             CellSpursTaskset **taskset);

#ifdef __cplusplus
}   /* extern "C" */

namespace cell {
namespace Spurs {

class Barrier : public CellSpursBarrier {
public:
    static const uint32_t kAlign = CELL_SPURS_BARRIER_ALIGN;
    static const uint32_t kSize  = CELL_SPURS_BARRIER_SIZE;

    static int initialize(CellSpursTaskset *taskset,
                          CellSpursBarrier *barrier,
                          unsigned int total)
    { return cellSpursBarrierInitialize(taskset, barrier, total); }

    int getTasksetAddress(CellSpursTaskset **taskset) const
    { return cellSpursBarrierGetTasksetAddress(this, taskset); }
};

}   /* namespace Spurs */
}   /* namespace cell */

#endif   /* __cplusplus */

#endif   /* __SPU__ */

#endif   /* __PS3DK_CELL_SPURS_BARRIER_H__ */
