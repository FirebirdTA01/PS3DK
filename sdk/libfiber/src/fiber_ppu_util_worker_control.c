/* cellFiberPpuUtilWorkerControlSendSignal / Wakeup - signal a fiber run by
 * a PPU fiber worker control, waking one of its sleeping workers. */
#include <cell/fiber/ppu_fiber_worker_control.h>
#include "ppu_fiber_signal.h"

int cellFiberPpuUtilWorkerControlSendSignal(uint32_t eaFiber, unsigned int *numWorker)
{
	fiber_signal_result r;
	int rc = __fiber_ppu_signal(eaFiber, 1, &r);
	if (rc)
		return rc;
	if (r.controlled == 1) {
		uint32_t control = (uint32_t)r.eaControl;
		if (control == 0)
			return FIBER_ERROR_NULL_POINTER;
		if (control & 0x7f)
			return FIBER_ERROR_ALIGN;
		return __fiber_worker_control_wakeup(control);
	}
	if (numWorker)
		*numWorker = r.numWorker;
	return 0;
}

int cellFiberPpuUtilWorkerControlWakeup(uint32_t eaControl)
{
	if (eaControl == 0)
		return FIBER_ERROR_NULL_POINTER;
	if (eaControl & 0x7f)
		return FIBER_ERROR_ALIGN;
	return __fiber_worker_control_wakeup(eaControl);
}
