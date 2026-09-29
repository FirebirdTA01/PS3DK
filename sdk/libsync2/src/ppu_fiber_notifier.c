/* gCellSync2NotifierPpuFiber - wakes a PPU fiber: receiver id = fiber EA.
 * The PPU fiber utility's SPU side comes from libfiber; without it a fiber
 * cannot have been made to wait, so reaching here halts. */
#include <cell/sync2/thread.h>
#include "sync2_internal.h"

extern int cellFiberPpuUtilWorkerControlSendSignal(uint32_t eaFiber, unsigned int *numWorker)
	__attribute__((weak));

static int ppu_fiber_send(CellSync2SignalReceiverId receiver, uint64_t arg)
{
	unsigned int numWorker;
	(void)arg;
	if (!cellFiberPpuUtilWorkerControlSendSignal
	    || cellFiberPpuUtilWorkerControlSendSignal((uint32_t)receiver, &numWorker) != 0)
		s2_halt();
	return 0;
}

CellSync2Notifier gCellSync2NotifierPpuFiber = {
	CELL_SYNC2_THREAD_TYPE_PPU_FIBER,
	ppu_fiber_send,
	0,
};
