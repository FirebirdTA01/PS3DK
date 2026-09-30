/* cell/fiber/ppu_fiber.h - signalling PPU fibers from the SPU (libfiber.a).
 *
 * A PPU fiber is named by its effective address.  cellFiberPpuSendSignal
 * wakes a fiber waiting for a signal (or leaves the signal pending);
 * numWorker, if not NULL, receives its scheduler's worker count.
 */
#ifndef __PS3DK_CELL_FIBER_PPU_FIBER_H_SPU__
#define __PS3DK_CELL_FIBER_PPU_FIBER_H_SPU__

#include <stdint.h>
#include <cell/fiber/error.h>
#include <cell/fiber/ppu_fiber_types.h>

#ifdef __cplusplus
extern "C" {
#endif

int cellFiberPpuSendSignal(uint32_t eaFiber, unsigned int *numWorker);
int cellFiberPpuGetScheduler(uint32_t eaFiber, uint32_t *pEaScheduler);

#ifdef __cplusplus
}
#endif

#endif /* __PS3DK_CELL_FIBER_PPU_FIBER_H_SPU__ */
