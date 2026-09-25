/* SPU fiber contexts: cooperative context switching inside one SPU program.
 *
 * A context is the 896-byte opaque CellFiberSpuContext.  This runtime uses:
 *   0x000-0x2ff  $80-$127 (callee-saved registers, context_switch.S)
 *   0x300        $0  link register - where the context resumes
 *   0x310        $1  stack pointer (word 0) and available stack (word 1)
 * A fresh context resumes in __ps3dk_fiber_start with the entry function
 * in $80, its argument in $81 and the context's own address in $82.
 */
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <cell/error.h>
#include <cell/fiber/error.h>
#include <cell/fiber/spu_context.h>

#define CTX_R80  0x000
#define CTX_R81  0x010
#define CTX_R82  0x020
#define CTX_LR   0x300
#define CTX_SP   0x310

/* The runtime's only use of a fiber's stack: the back-chain quadword and
 * the link-register slot the entry function's prologue writes, at the top.
 * Everything below belongs to the entry function, as with any stack. */
#define STACK_TOP_RESERVE 32
/* The reserve plus one minimal (32-byte) frame for the entry function. */
#define STACK_MIN 64

void __ps3dk_fiber_swap(CellFiberSpuContext *save, CellFiberSpuContext *load);
void __ps3dk_fiber_start(void);

/* The running fiber, or NULL in ordinary code; read and cleared by
 * __ps3dk_fiber_start as well. */
CellFiberSpuContext *__ps3dk_fiber_current __attribute__((visibility("hidden")));
/* Where ordinary code resumes when a fiber's entry function returns. */
CellFiberSpuContext *__ps3dk_fiber_run_caller __attribute__((visibility("hidden")));

static void put_word(CellFiberSpuContext *c, unsigned off, uint32_t w0, uint32_t w1)
{
    uint32_t q[4] = { w0, w1, 0, 0 };
    memcpy(c->skip + off, q, sizeof q);
}

int cellFiberSpuContextInitialize(CellFiberSpuContext *fiber,
                                  CellFiberSpuContextEntry entry,
                                  uint64_t arg,
                                  void *stack,
                                  size_t stackSize)
{
    if (fiber == NULL || entry == NULL || stack == NULL)
        return CELL_FIBER_ERROR_NULL_POINTER;
    if (((uintptr_t)fiber & (CELL_FIBER_SPU_CONTEXT_ALIGN - 1)) != 0
            || ((uintptr_t)stack & (CELL_FIBER_SPU_CONTEXT_STACK_ALIGN - 1)) != 0
            || (stackSize & (CELL_FIBER_SPU_CONTEXT_STACK_ALIGN - 1)) != 0)
        return CELL_FIBER_ERROR_ALIGN;
    if (stackSize < STACK_MIN)
        return CELL_FIBER_ERROR_INVAL;

    memset(fiber, 0, sizeof *fiber);

    uint32_t top = (uint32_t)(uintptr_t)stack + (uint32_t)stackSize;
    uint32_t sp = top - STACK_TOP_RESERVE;
    /* A zero back chain ends the frame list for debuggers and unwinders. */
    memset((void *)(uintptr_t)sp, 0, STACK_TOP_RESERVE);

    put_word(fiber, CTX_R80, (uint32_t)(uintptr_t)entry, 0);
    put_word(fiber, CTX_R81, (uint32_t)(arg >> 32), (uint32_t)arg);
    put_word(fiber, CTX_R82, (uint32_t)(uintptr_t)fiber, 0);
    put_word(fiber, CTX_LR, (uint32_t)(uintptr_t)__ps3dk_fiber_start, 0);
    put_word(fiber, CTX_SP, sp, sp - (uint32_t)(uintptr_t)stack);
    return CELL_OK;
}

int cellFiberSpuContextRun(CellFiberSpuContext *fiber,
                           CellFiberSpuContext *callerContext)
{
    if (fiber == NULL || callerContext == NULL)
        return CELL_FIBER_ERROR_NULL_POINTER;
    if (((uintptr_t)fiber & (CELL_FIBER_SPU_CONTEXT_ALIGN - 1)) != 0
            || ((uintptr_t)callerContext & (CELL_FIBER_SPU_CONTEXT_ALIGN - 1)) != 0)
        return CELL_FIBER_ERROR_ALIGN;
    if (__ps3dk_fiber_current != NULL)
        return CELL_FIBER_ERROR_PERM;   /* already running fibers */

    __ps3dk_fiber_run_caller = callerContext;
    __ps3dk_fiber_current = fiber;
    __ps3dk_fiber_swap(callerContext, fiber);
    /* Resumed by __ps3dk_fiber_start after a fiber's entry function returned
     * (it has already cleared __ps3dk_fiber_current). */
    return CELL_OK;
}

CellFiberSpuContext *cellFiberSpuContextSelf(void)
{
    return __ps3dk_fiber_current;
}

void _cellFiberSpuContextSwitch(CellFiberSpuContext *context)
{
    CellFiberSpuContext *self = __ps3dk_fiber_current;
    __ps3dk_fiber_current = context;
    __ps3dk_fiber_swap(self, context);
    /* Resumed: whoever switched back already set the current fiber. */
}
