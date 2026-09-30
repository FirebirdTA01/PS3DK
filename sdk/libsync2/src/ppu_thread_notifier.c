/* gCellSync2NotifierPpuThread - wakes a PPU thread waiting on an lv2 event
 * flag: receiver id = bit number << 32 | event flag id.
 *
 * The bit is set with the impatient (non-blocking) event flag syscall,
 * issued here directly so that jobs linking libsync2 need no libsputhread:
 * the flag id goes to the outbound mailbox, then bit | 0xc0000000 to the
 * outbound interrupt mailbox. */
#include <cell/sync2/thread.h>
#include "sync2_internal.h"

static int ppu_thread_send(CellSync2SignalReceiverId receiver, uint64_t arg)
{
	(void)arg;
	spu_writech(SPU_WrOutMbox, (uint32_t)receiver);
	spu_writech(SPU_WrOutIntrMbox, ((uint32_t)(receiver >> 32) & 63) | 0xc0000000u);
	return 0;
}

CellSync2Notifier gCellSync2NotifierPpuThread = {
	CELL_SYNC2_THREAD_TYPE_PPU_THREAD,
	ppu_thread_send,
	0,
};
