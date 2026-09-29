/* gCellSync2NotifierPpuThread - wakes a PPU thread waiting on an lv2 event
 * flag: receiver id = bit number << 32 | event flag id. */
#include <cell/sync2/thread.h>
#include <sys/event_flag.h>
#include "sync2_internal.h"

static int ppu_thread_send(CellSync2SignalReceiverId receiver, uint64_t arg)
{
	(void)arg;
	if (sys_event_flag_set_bit_impatient((uint32_t)receiver, (receiver >> 32) & 63) != 0)
		s2_halt();
	return 0;
}

CellSync2Notifier gCellSync2NotifierPpuThread = {
	CELL_SYNC2_THREAD_TYPE_PPU_THREAD,
	ppu_thread_send,
	0,
};
