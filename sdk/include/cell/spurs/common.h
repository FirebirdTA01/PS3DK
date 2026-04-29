/* cell/spurs/common.h - SPU-side common runtime entry points.
 *
 * Thin getters that any policy module / task / workload running on
 * the SPU calls to discover its execution context.  These are
 * SPU-only (PPU code uses different APIs to read the same fields
 * via the CellSpurs PPU object).  Implementations live in
 * libspurs_task.a (sdk/libspurs_task/src/spurs_module_runtime.c).
 *
 * cellSpursPoll() / cellSpursModulePoll() / cellSpursModulePollStatus()
 * cooperative-yield helpers are declared here too but live in
 * sdk/libspurs_task/src/spurs_module_runtime.c when implemented; the
 * current shipped subset is the four context getters below.
 */
#ifndef __PS3DK_CELL_SPURS_COMMON_H__
#define __PS3DK_CELL_SPURS_COMMON_H__

#include <stdint.h>
#include <stdbool.h>

#include <cell/spurs/types.h>

#ifdef __cplusplus
extern "C" {
#endif

/* SPU-side context getters - return values populated by the SPRX
 * SPURS kernel before each workload / task is dispatched. */
extern uint32_t            cellSpursGetCurrentSpuId(void);
extern uint64_t            cellSpursGetSpursAddress(void);
extern uint32_t            cellSpursGetTagId(void);
extern CellSpursWorkloadId cellSpursGetWorkloadId(void);
extern uint32_t            cellSpursGetSpuCount(void);

/* SPU-side dispatch helpers - call into the SPRX kernel via the
 * per-module / per-task dispatch tables.  See implementation notes
 * in sdk/libspurs_task/src/spurs_module_runtime.c. */

/* Per-task ELF address (uint64_t EA in main memory) - populated by
 * the kernel at task dispatch.  Returns 0 outside a task context. */
extern uint64_t            cellSpursGetElfAddress(void);

/* In-Workload (IWL) packed task identifier:
 *   (workloadId << 8) | (taskId & 0xff)
 * Used by trace + workload-aware paths. */
extern uint32_t            _cellSpursGetIWLTaskId(void);

/* Cooperative-yield / status checks.  cellSpursPoll +
 * cellSpursModulePoll return non-zero when the SPU should yield to
 * higher-priority work; cellSpursModulePollStatus additionally
 * writes the kernel's raw workload-id response to *pStatus. */
extern bool                cellSpursPoll(void);
extern int                 cellSpursModulePoll(void);
extern int                 cellSpursModulePollStatus(uint32_t *pStatus);

/* Tear the running module down via the kernel.  Does not return. */
extern void                cellSpursModuleExit(void) __attribute__((noreturn));

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* __PS3DK_CELL_SPURS_COMMON_H__ */
