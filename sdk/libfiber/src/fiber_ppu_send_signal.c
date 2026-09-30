/* cellFiberPpuSendSignal - signal a PPU fiber from the SPU.  numWorker
 * (optional) receives the scheduler's worker count. */
#include <cell/fiber/ppu_fiber.h>
#include "ppu_fiber_signal.h"

int cellFiberPpuSendSignal(uint32_t eaFiber, unsigned int *numWorker)
{
	fiber_signal_result r;
	int rc = __fiber_ppu_signal(eaFiber, numWorker != 0, &r);
	if (rc)
		return rc;
	if (numWorker)
		*numWorker = r.numWorker;
	return 0;
}
