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

#endif   /* __PS3DK_CELL_SPURS_BARRIER_H__ */
