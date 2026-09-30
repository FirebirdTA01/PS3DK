/* ppu_fiber_signal.c - signal a PPU fiber from the SPU (see ppu_fiber_signal.h). */
#include "ppu_fiber_signal.h"

static volatile uint8_t s_line[128] __attribute__((aligned(128)));
static volatile uint32_t s_word[4] __attribute__((aligned(16)));

#define barrier() __asm__ volatile("" ::: "memory")

static void getllar(uint32_t ea)
{
	mfc_getllar(s_line, ea, 0, 0);
	mfc_read_atomic_status();
	barrier();
}

static int putllc(uint32_t ea)
{
	barrier();
	spu_dsync();
	mfc_putllc(s_line, ea, 0, 0);
	return (mfc_read_atomic_status() & MFC_PUTLLC_STATUS) == 0;
}

#define U8(o)  (*(volatile uint8_t *)(s_line + (o)))
#define U32(o) (*(volatile uint32_t *)(s_line + (o)))
#define U64(o) (*(volatile uint64_t *)(s_line + (o)))

int __fiber_ppu_signal(uint32_t eaFiber, int readWorkers, fiber_signal_result *out)
{
	uint32_t state, waiting, scheduler, index;

	if (eaFiber == 0)
		return FIBER_ERROR_NULL_POINTER;
	if (eaFiber & 0x7f)
		return FIBER_ERROR_ALIGN;

	do {
		getllar(eaFiber);
		state = U32(0);
		if (state == 0 || (state & FIBER_CLOSED))
			return FIBER_ERROR_STAT;
		waiting = state & FIBER_WAITING;
		U32(0) = waiting ? ((state & ~FIBER_WAITING) | FIBER_WOKEN) : (state | FIBER_PENDING);
	} while (!putllc(eaFiber));
	scheduler = U32(0x08);
	out->controlled = U32(0x10);
	out->eaControl = U64(0x18);
	index = U8(0x0c);
	out->numWorker = 0;

	if (waiting) {
		/* push the fiber onto its ready list and flag the list */
		do {
			getllar(scheduler);
			volatile uint32_t *next = &s_word[1];
			*next = U32(index * 4);
			barrier();
			mfc_put(next, eaFiber + 4, 4, 0, 0, 0);
			mfc_write_tag_mask(1u << 0);
			mfc_read_tag_status_all();
			U32(index * 4) = eaFiber;
			U8(0x48 + index) = 0xff;
			out->numWorker = U8(0x40);
		} while (!putllc(scheduler));
	} else if (readWorkers) {
		getllar(scheduler);
		out->numWorker = U8(0x40);
	}
	return 0;
}

int __fiber_worker_control_wakeup(uint32_t eaControl)
{
	const uint32_t ea = eaControl + WORKER_CONTROL_LINE;
	uint32_t bit, n;

	do {
		getllar(ea);
		if (!(U8(3) & 0x40))
			return FIBER_ERROR_STAT;
		bit = 0;
		n = 0;
		if (U8(2) == 0) {
			uint32_t sleeping = U32(4);
			n = __builtin_clz(~sleeping);
			bit = sleeping ? 1u << n : 0;
			U32(4) = sleeping & ~bit;
		}
		U8(2) = 1;
	} while (!putllc(ea));

	if (!bit)
		return 0;
	/* sys_event_flag_set_bit_impatient(flag, n): flag id to the outbound
	 * mailbox, then the bit | 0xc0000000 to the outbound interrupt mailbox */
	spu_writech(SPU_WrOutMbox, U32(8));
	spu_writech(SPU_WrOutIntrMbox, (n & 63) | 0xc0000000u);
	return 0;
}
