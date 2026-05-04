/* cell/fiber/ppu_context_types.h - CellFiberPpuContext opaque types.
 *
 * Clean-room header.  CellFiberPpuContext is the low-level fiber
 * primitive (no scheduler, just context-switch).  The SPRX owns the
 * live state; PPU-visible storage is an opaque byte array of the
 * size the SPRX expects.
 */
#ifndef __PS3DK_CELL_FIBER_PPU_CONTEXT_TYPES_H__
#define __PS3DK_CELL_FIBER_PPU_CONTEXT_TYPES_H__

#include <stdint.h>
#include <stdbool.h>

/* ATTRIBUTE_PRXPTR shrinks cross-SPRX pointer fields from LP64
 * 8-byte storage to the 4-byte EA the SPRX expects.  On SPU it is a
 * no-op because pointers are already 32-bit. */
#ifdef __PPU__
#include <ppu-types.h>
#else
#ifndef ATTRIBUTE_PRXPTR
#define ATTRIBUTE_PRXPTR
#endif
#endif

/* Forward declaration — opaque execution-option struct consumed by
 * ContextRun / ContextSwitch. */
typedef struct CellFiberPpuContextExecutionOption CellFiberPpuContextExecutionOption;

#define CELL_FIBER_PPU_CONTEXT_STACK_ALIGN  16

#define CELL_FIBER_PPU_CONTEXT_ALIGN  16
#define CELL_FIBER_PPU_CONTEXT_SIZE   (128 * 5)

#define CELL_FIBER_PPU_CONTEXT_NAME_MAX_LENGTH  31

#define CELL_FIBER_PPU_CONTEXT_ATTRIBUTE_ALIGN  8
#define CELL_FIBER_PPU_CONTEXT_ATTRIBUTE_SIZE   128

typedef struct CellFiberPpuContextAttribute {
    uint8_t privateHeader[16];
    char    name[CELL_FIBER_PPU_CONTEXT_NAME_MAX_LENGTH + 1];
    bool    debuggerSupport;

    uint8_t skip[CELL_FIBER_PPU_CONTEXT_ATTRIBUTE_SIZE - 16
                 - (CELL_FIBER_PPU_CONTEXT_NAME_MAX_LENGTH + 1)
                 - sizeof(bool)];
} __attribute__((aligned(CELL_FIBER_PPU_CONTEXT_ATTRIBUTE_ALIGN))) CellFiberPpuContextAttribute;

typedef struct CellFiberPpuContext {
    unsigned char skip[CELL_FIBER_PPU_CONTEXT_SIZE];
} __attribute__((aligned(CELL_FIBER_PPU_CONTEXT_ALIGN))) CellFiberPpuContext;

/* The context entry-point callback.  arg is the user value passed at
 * initialize time; fiberFrom is the context that called Run/Switch. */
typedef void (*CellFiberPpuContextEntry)(
    uint64_t                arg,
    CellFiberPpuContext    *fiberFrom
);

#endif /* __PS3DK_CELL_FIBER_PPU_CONTEXT_TYPES_H__ */
