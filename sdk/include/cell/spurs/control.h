/*
 * PS3 Custom Toolchain - cell/spurs/control.h
 *
 * PPU: alias for the reference-SDK <cell/spurs/control.h> include path;
 * the PPU declarations live in <cell/spurs/types.h>.
 * SPU: workload priority and contention control from code running under
 * SPURS (libspurs.a).
 */
#ifndef _PS3DK_CELL_SPURS_CONTROL_H_
#define _PS3DK_CELL_SPURS_CONTROL_H_
#include <cell/spurs/types.h>

#ifdef __SPU__
#include <spu_intrinsics.h>
#include <cell/spurs/error.h>

#ifdef __cplusplus
extern "C" {
#endif

/* at most `maxContention` (1..8) SPUs run workload `id` at once */
int cellSpursSetMaxContention(CellSpursWorkloadId id, unsigned int maxContention);
/* bytes 0..7: the workload's priority (0..15) on SPUs 0..7 */
int cellSpursSetPriorities(CellSpursWorkloadId id, vec_uchar16 priorities);
int cellSpursSetPriority(CellSpursWorkloadId id, unsigned spu, unsigned priority);

#ifdef __cplusplus
}   /* extern "C" */
#endif
#endif /* __SPU__ */

#endif
