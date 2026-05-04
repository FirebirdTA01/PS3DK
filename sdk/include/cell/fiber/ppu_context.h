/* cell/fiber/ppu_context.h - CellFiberPpuContext API.
 *
 * Clean-room header.  CellFiberPpuContext is the low-level
 * co-operative fiber primitive — no scheduler, just initialize /
 * finalize / switch / return-to-thread.  Callers manage their own
 * scheduling policy on top.
 */
#ifndef __PS3DK_CELL_FIBER_PPU_CONTEXT_H__
#define __PS3DK_CELL_FIBER_PPU_CONTEXT_H__

#include <stdint.h>
#include <stddef.h>

#include <cell/fiber/ppu_context_types.h>
#include <cell/fiber/ppu_initialize.h>
#include <cell/fiber/version.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Attribute initializer (underscored — forward to SPRX with version). */
int _cellFiberPpuContextAttributeInitialize(
    CellFiberPpuContextAttribute *attr,
    uint32_t                      sdkVersion
);

static inline int
cellFiberPpuContextAttributeInitialize(
    CellFiberPpuContextAttribute *attr
)
{
    return _cellFiberPpuContextAttributeInitialize(attr,
                                                   _CELL_FIBER_PPU_INTERNAL_VERSION);
}

int cellFiberPpuContextInitialize(
    CellFiberPpuContext               *fiber,
    CellFiberPpuContextEntry           entry,
    uint64_t                           arg,
    void                              *eaStack,
    size_t                             stackSize,
    const CellFiberPpuContextAttribute *attr
);

int cellFiberPpuContextFinalize(CellFiberPpuContext *fiber);

int cellFiberPpuContextRun(
    CellFiberPpuContext                     *fiberTo,
    int                                     *cause,
    CellFiberPpuContext                    **fiberFrom,
    const CellFiberPpuContextExecutionOption *option
);

int cellFiberPpuContextSwitch(
    CellFiberPpuContext                     *fiberTo,
    CellFiberPpuContext                    **fiberFrom,
    const CellFiberPpuContextExecutionOption *option
);

int cellFiberPpuContextReturnToThread(int cause);

CellFiberPpuContext *cellFiberPpuContextSelf(void);

int cellFiberPpuContextCheckStackLimit(void);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* __PS3DK_CELL_FIBER_PPU_CONTEXT_H__ */
