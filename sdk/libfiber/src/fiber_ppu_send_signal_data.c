/* _cellFiberPpuSendSignalAndGetData - cellFiberPpuSendSignal that also
 * returns the fiber's worker-control words (for the worker control's
 * signal path). */
#include <stdint.h>
#include "ppu_fiber_signal.h"

int _cellFiberPpuSendSignalAndGetData(uint32_t eaFiber, unsigned int *numWorker, uint32_t *controlled,
                                      uint64_t *eaControl)
{
	fiber_signal_result r;
	int rc = __fiber_ppu_signal(eaFiber, numWorker != 0, &r);
	if (rc)
		return rc;
	if (numWorker)
		*numWorker = r.numWorker;
	if (controlled)
		*controlled = r.controlled;
	if (eaControl)
		*eaControl = r.eaControl;
	return 0;
}
