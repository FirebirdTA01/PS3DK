/* cell/fiber/user_scheduler.h - user scheduler API + trace entry points.
 *
 * Clean-room header.  The user scheduler runs a caller-provided
 * callback inside a fiber context, letting the application implement
 * custom scheduling policies.  Trace entry points for scheduler-level
 * event recording are co-located here.
 */
#ifndef __PS3DK_CELL_FIBER_USER_SCHEDULER_H__
#define __PS3DK_CELL_FIBER_USER_SCHEDULER_H__

#include <stdint.h>
#include <stddef.h>

#include <cell/fiber/error.h>
#include <cell/fiber/ppu_context_types.h>
#include <cell/fiber/user_scheduler_types.h>
#include <cell/fiber/ppu_fiber_types.h>
#include <cell/fiber/ppu_fiber_trace_types.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---- user scheduler --------------------------------------------- */

int cellFiberPpuContextRunScheduler(
    CellFiberPpuSchedulerCallback            scheduler,
    uint64_t                                 arg0,
    uint64_t                                 arg1,
    int                                     *cause,
    CellFiberPpuContext                    **fiberFrom,
    const CellFiberPpuContextExecutionOption *option
);

int cellFiberPpuContextEnterScheduler(
    CellFiberPpuSchedulerCallback            scheduler,
    uint64_t                                 arg0,
    uint64_t                                 arg1,
    CellFiberPpuContext                    **fiberFrom,
    const CellFiberPpuContextExecutionOption *option
);

/* ---- scheduler trace -------------------------------------------- */

int cellFiberPpuSchedulerTraceInitialize(
    CellFiberPpuScheduler *scheduler,
    void                  *buffer,
    size_t                 size,
    uint32_t               mode
);

int cellFiberPpuSchedulerTraceStart(CellFiberPpuScheduler *scheduler);

int cellFiberPpuSchedulerTraceStop(CellFiberPpuScheduler *scheduler);

int cellFiberPpuSchedulerTraceFinalize(CellFiberPpuScheduler *scheduler);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* __PS3DK_CELL_FIBER_USER_SCHEDULER_H__ */
