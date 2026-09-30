/* gCellSync2NotifierPpuFiber - wakes a PPU fiber through its fiber utility
 * worker control: receiver id = fiber EA (libfiber). */
#include <cell/sync2/thread.h>
#include "sync2_internal.h"

extern int cellFiberPpuUtilWorkerControlSendSignal(uint32_t eaFiber, unsigned int *numWorker);

static int ppu_fiber_send(CellSync2SignalReceiverId receiver, uint64_t arg)
{
	unsigned int numWorker;
	(void)arg;
	if (cellFiberPpuUtilWorkerControlSendSignal((uint32_t)receiver, &numWorker) != 0)
		s2_halt();
	return 0;
}

CellSync2Notifier gCellSync2NotifierPpuFiber = {
	CELL_SYNC2_THREAD_TYPE_PPU_FIBER,
	ppu_fiber_send,
	0,
};
