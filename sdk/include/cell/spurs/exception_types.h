/* cell/spurs/exception_types.h - SPURS exception-info struct + handler
 * function-pointer typedefs.
 *
 * Used by job-chain + workload + global exception handlers.  The 24-byte
 * CellSpursExceptionInfo layout matches the SPRX wire shape.
 */

/* Outside the guard: in C++, cell/spurs/types.h ends with class wrappers
 * that include this header (through cell/spurs/exception.h) and use its
 * handler types.  Entering types.h first lets that inner include define
 * them before the wrappers need them; this file's own pass is then a
 * no-op. */
#include <cell/spurs/types.h>

#ifndef __PS3DK_CELL_SPURS_EXCEPTION_TYPES_H__
#define __PS3DK_CELL_SPURS_EXCEPTION_TYPES_H__

#include <stdint.h>
#ifndef __SPU__
#include <sys/types.h>
#include <sys/spu_thread.h>
#endif

#ifdef __cplusplus
extern "C" {
#endif

/* size: 24 bytes.  spu_thread field is the PPU sys_spu_thread_t (an
 * unsigned 32-bit on PS3); SPU-side code only ever sees this struct
 * as an exception-event payload DMA'd from main memory and treats
 * the field as a plain uint32_t. */
typedef struct CellSpursExceptionInfo {
#ifdef __SPU__
    uint32_t         spu_thread;
#else
    sys_spu_thread_t spu_thread;
#endif
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
