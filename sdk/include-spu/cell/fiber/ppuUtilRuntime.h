/* cell/fiber/ppuUtilRuntime.h - SPU view of a PPU fiber utility runtime:
 * wake one of its sleeping workers, or signal one of its fibers. */
#ifndef __PS3DK_CELL_FIBER_PPU_UTIL_RUNTIME_H_SPU__
#define __PS3DK_CELL_FIBER_PPU_UTIL_RUNTIME_H_SPU__

#include <stdint.h>
#include <cell/fiber/ppu_fiber_types.h>
#include <cell/fiber/ppu_fiber.h>
#include <cell/fiber/ppu_fiber_worker_control.h>
#include <cell/fiber/ppuUtilDefine.h>

#ifdef __cplusplus
__CELL_FIBER_PPU_UTIL_BEGIN

class Runtime {
public:
	static int wakeup(uint64_t eaRuntime)
	{
		return cellFiberPpuUtilWorkerControlWakeup((uint32_t)eaRuntime);
	}
	static int sendSignal(uint32_t eaFiber, unsigned *numWorker = 0)
	{
		return cellFiberPpuUtilWorkerControlSendSignal(eaFiber, numWorker);
	}
};

__CELL_FIBER_PPU_UTIL_END
#endif

#endif /* __PS3DK_CELL_FIBER_PPU_UTIL_RUNTIME_H_SPU__ */
