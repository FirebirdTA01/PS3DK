/* cell/fiber/spu_context.h -- SPU fiber contexts (cooperative context
 * switching inside one SPU program).
 *
 *   cellFiberSpuContextInitialize  prepare a context to run entry(arg) on
 *                                  its own stack
 *   cellFiberSpuContextRun         from ordinary (non-fiber) code, save the
 *                                  caller in callerContext and start running
 *                                  fibers; returns when a fiber's entry
 *                                  function returns
 *   cellFiberSpuContextSwitch      from inside a fiber, save the current
 *                                  fiber and resume another
 *   cellFiberSpuContextSelf        the running fiber, or NULL outside one
 *
 * Implemented in libfiber.a (-lfiber).
 */
#ifndef __PS3DK_CELL_FIBER_SPU_CONTEXT_H__
#define __PS3DK_CELL_FIBER_SPU_CONTEXT_H__

#include <stddef.h>
#include <stdint.h>
#include <cell/error.h>
#include <cell/fiber/error.h>
#include <cell/fiber/spu_context_types.h>

#ifdef __cplusplus
extern "C" {
#endif

int cellFiberSpuContextInitialize(CellFiberSpuContext *fiber,
                                  CellFiberSpuContextEntry entry,
                                  uint64_t arg,
                                  void *stack,
                                  size_t stackSize);

int cellFiberSpuContextRun(CellFiberSpuContext *fiber,
                           CellFiberSpuContext *callerContext);

CellFiberSpuContext *cellFiberSpuContextSelf(void);

/* The unchecked switch; cellFiberSpuContextSwitch validates first. */
void _cellFiberSpuContextSwitch(CellFiberSpuContext *context);

static inline __attribute__((always_inline))
int cellFiberSpuContextSwitch(CellFiberSpuContext *context)
{
#ifndef CELL_FIBER_SPU_CONTEXT_SWITCH_NO_ERROR_CHECK
    if (__builtin_expect(context == NULL, 0))
        return CELL_FIBER_ERROR_NULL_POINTER;
    if (__builtin_expect(((uintptr_t)context & (CELL_FIBER_SPU_CONTEXT_ALIGN - 1)) != 0, 0))
        return CELL_FIBER_ERROR_ALIGN;
    if (__builtin_expect(cellFiberSpuContextSelf() == NULL, 0))
        return CELL_FIBER_ERROR_PERM;
#endif
    _cellFiberSpuContextSwitch(context);
    return CELL_OK;
}

#ifdef __cplusplus
}
#endif

#endif /* __PS3DK_CELL_FIBER_SPU_CONTEXT_H__ */
