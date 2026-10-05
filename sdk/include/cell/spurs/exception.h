/* cell/spurs/exception.h - SPURS exception event handlers (PPU).
 *
 * A handler runs on the PPU when an SPU workload faults: per workload
 * (Set/Unset), or for the whole SPURS instance (SetGlobal/UnsetGlobal).
 * EnableExceptionEventHandler switches delivery on and off.
 */

/* Outside the guard: in C++, cell/spurs/types.h ends with class wrappers
 * that include this header and call its functions.  Entering types.h
 * first lets that inner include declare them before the wrappers need
 * them; this file's own pass is then a no-op. */
#include <cell/spurs/types.h>

#ifndef __PS3DK_CELL_SPURS_EXCEPTION_H__
#define __PS3DK_CELL_SPURS_EXCEPTION_H__

#include <stdint.h>
#include <stdbool.h>
#include <cell/spurs/error.h>
#include <cell/spurs/exception_types.h>

#ifdef __cplusplus
extern "C" {
#endif

extern int cellSpursEnableExceptionEventHandler(CellSpurs *spurs, bool flag);
extern int cellSpursSetExceptionEventHandler(CellSpurs *spurs, CellSpursWorkloadId id,
                                             CellSpursExceptionEventHandler eaHandler, void *arg);
extern int cellSpursUnsetExceptionEventHandler(CellSpurs *spurs, CellSpursWorkloadId id);
extern int cellSpursSetGlobalExceptionEventHandler(CellSpurs *spurs,
                                                   CellSpursGlobalExceptionEventHandler eaHandler, void *arg);
extern int cellSpursUnsetGlobalExceptionEventHandler(CellSpurs *spurs);

#ifdef __cplusplus
}   /* extern "C" */
#endif

#endif /* __PS3DK_CELL_SPURS_EXCEPTION_H__ */
