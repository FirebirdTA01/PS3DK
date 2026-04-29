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

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* __PS3DK_CELL_SPURS_COMMON_H__ */
