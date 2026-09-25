/* cell/fiber/spu_context_types.h -- SPU fiber context types.
 *
 * Shared by PPU and SPU code (a PPU program may reserve or DMA the
 * context storage).  The context is opaque storage; its layout belongs to
 * the SPU runtime in libfiber.a.
 */
#ifndef __PS3DK_CELL_FIBER_SPU_CONTEXT_TYPES_H__
#define __PS3DK_CELL_FIBER_SPU_CONTEXT_TYPES_H__

#include <stdint.h>

#define CELL_FIBER_SPU_CONTEXT_SIZE         (128 * 7)
#define CELL_FIBER_SPU_CONTEXT_ALIGN        16
#define CELL_FIBER_SPU_CONTEXT_STACK_ALIGN  16

typedef struct CellFiberSpuContext {
    uint8_t skip[CELL_FIBER_SPU_CONTEXT_SIZE];
} __attribute__((aligned(CELL_FIBER_SPU_CONTEXT_ALIGN))) CellFiberSpuContext;

typedef void (*CellFiberSpuContextEntry)(uint64_t arg);

#endif /* __PS3DK_CELL_FIBER_SPU_CONTEXT_TYPES_H__ */
