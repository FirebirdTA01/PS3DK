/* cell/spurs/exception_types.h - SPURS exception-info struct + handler
 * function-pointer typedefs.
 *
 * Used by job-chain + workload + global exception handlers.  The 24-byte
 * CellSpursExceptionInfo layout matches the SPRX wire shape.
 */
#ifndef __PS3DK_CELL_SPURS_EXCEPTION_TYPES_H__
#define __PS3DK_CELL_SPURS_EXCEPTION_TYPES_H__

#include <stdint.h>
#include <sys/types.h>
#include <sys/spu_thread.h>
#include <cell/spurs/types.h>

#ifdef __cplusplus
extern "C" {
#endif

/* size: 24 bytes */
typedef struct CellSpursExceptionInfo {
    sys_spu_thread_t spu_thread;
    uint32_t         spu_npc;
    uint32_t         cause;
    /* 4 bytes padding here */
    uint64_t         option;
} CellSpursExceptionInfo;

typedef void (*CellSpursExceptionEventHandler)(
    CellSpurs *,
    const CellSpursExceptionInfo *,
    void *);

typedef void (*CellSpursGlobalExceptionEventHandler)(
    CellSpurs *,
    const CellSpursExceptionInfo *,
    CellSpursWorkloadId,
    void *);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* __PS3DK_CELL_SPURS_EXCEPTION_TYPES_H__ */
