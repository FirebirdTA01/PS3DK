/* cell/fiber/user_scheduler_types.h - user scheduler callback typedef.
 *
 * Clean-room header.  The user scheduler callback is the core of the
 * co-operative scheduler interface — it is invoked by
 * cellFiberPpuContextRunScheduler / EnterScheduler each time a fiber
 * yields or exits.
 */
#ifndef __PS3DK_CELL_FIBER_USER_SCHEDULER_TYPES_H__
#define __PS3DK_CELL_FIBER_USER_SCHEDULER_TYPES_H__

#include <stdint.h>
#include <cell/fiber/ppu_context_types.h>

typedef CellFiberPpuContext *(*CellFiberPpuSchedulerCallback)(
    uint64_t arg0,
    uint64_t arg1
);

#endif /* __PS3DK_CELL_FIBER_USER_SCHEDULER_TYPES_H__ */
