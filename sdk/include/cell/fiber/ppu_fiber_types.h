/* cell/fiber/ppu_fiber_types.h - CellFiberPpu and scheduler opaque types.
 *
 * Clean-room header.  CellFiberPpu is the high-level fiber primitive
 * that lives on top of a CellFiberPpuScheduler.  The on-exit callback
 * carries ATTRIBUTE_PRXPTR so the function-pointer field is 4 bytes
 * wide (matching the SPRX EA), followed by a 4-byte reserved pad to
 * keep the struct layout at 256 bytes total.
 */
#ifndef __PS3DK_CELL_FIBER_PPU_FIBER_TYPES_H__
#define __PS3DK_CELL_FIBER_PPU_FIBER_TYPES_H__

#include <stdint.h>
#include <stdbool.h>

#include <cell/fiber/ppu_context_types.h>

/* ATTRIBUTE_PRXPTR shrink for cross-SPRX pointer fields. */
#ifdef __PPU__
#include <ppu-types.h>
#else
#ifndef ATTRIBUTE_PRXPTR
#define ATTRIBUTE_PRXPTR
#endif
#endif

/* ---- fiber ------------------------------------------------------ */

typedef void (*CellFiberPpuOnExitCallback)(uint64_t arg, int exitCode);

#define CELL_FIBER_PPU_NAME_MAX_LENGTH  CELL_FIBER_PPU_CONTEXT_NAME_MAX_LENGTH

#define CELL_FIBER_PPU_ATTRIBUTE_ALIGN  8
#define CELL_FIBER_PPU_ATTRIBUTE_SIZE   256

typedef struct CellFiberPpuAttribute {
    uint8_t privateHeader[16];

    char name[CELL_FIBER_PPU_NAME_MAX_LENGTH + 1];

    CellFiberPpuOnExitCallback onExitCallback ATTRIBUTE_PRXPTR;
    uint32_t                   __reserved0__;    /* ABI pad — MUST be 0 */
    uint64_t                   onExitCallbackArg;

    uint64_t                   __reserved1__;    /* ABI pad — MUST be 0 */

    uint8_t skip[CELL_FIBER_PPU_ATTRIBUTE_SIZE - 16
                 - (CELL_FIBER_PPU_NAME_MAX_LENGTH + 1)
                 - sizeof(CellFiberPpuOnExitCallback ATTRIBUTE_PRXPTR)
                 - sizeof(uint32_t)
                 - sizeof(uint64_t) * 2];
} __attribute__((aligned(CELL_FIBER_PPU_ATTRIBUTE_ALIGN))) CellFiberPpuAttribute;

#define CELL_FIBER_PPU_STACK_ALIGN  16

#define CELL_FIBER_PPU_ALIGN  128
#define CELL_FIBER_PPU_SIZE   (CELL_FIBER_PPU_CONTEXT_SIZE + 128 * 2)

typedef struct CellFiberPpu {
    unsigned char skip[CELL_FIBER_PPU_SIZE];
} __attribute__((aligned(CELL_FIBER_PPU_ALIGN))) CellFiberPpu;

/* ---- fiber scheduler -------------------------------------------- */

#define CELL_FIBER_PPU_SCHEDULER_ATTRIBUTE_ALIGN  8
#define CELL_FIBER_PPU_SCHEDULER_ATTRIBUTE_SIZE   256

typedef struct CellFiberPpuSchedulerAttribute {
    uint8_t  privateHeader[16];

    bool     autoCheckFlags;
    bool     debuggerSupport;
    uint8_t  padding[2];
    uint32_t autoCheckFlagsIntervalUsec;

    uint8_t  skip[CELL_FIBER_PPU_SCHEDULER_ATTRIBUTE_SIZE - 16
                  - sizeof(bool) * 2
                  - 2
                  - sizeof(uint32_t)];
} __attribute__((aligned(CELL_FIBER_PPU_SCHEDULER_ATTRIBUTE_ALIGN))) CellFiberPpuSchedulerAttribute;

#define CELL_FIBER_PPU_NUM_PRIORITY      4
#define CELL_FIBER_PPU_LOWEST_PRIORITY   (CELL_FIBER_PPU_NUM_PRIORITY - 1)

#define CELL_FIBER_PPU_SCHEDULER_ALIGN  128
#define CELL_FIBER_PPU_SCHEDULER_SIZE   (128 * 4)

typedef struct CellFiberPpuScheduler {
    unsigned char skip[CELL_FIBER_PPU_SCHEDULER_SIZE];
} __attribute__((aligned(CELL_FIBER_PPU_SCHEDULER_ALIGN))) CellFiberPpuScheduler;

/* ---- entry-point typedef ---------------------------------------- */

typedef int (*CellFiberPpuEntry)(uint64_t arg);

#endif /* __PS3DK_CELL_FIBER_PPU_FIBER_TYPES_H__ */
