/* cellFiberPpuGetScheduler - the EA of a PPU fiber's scheduler. */
#include <cell/fiber/ppu_fiber.h>
#include <spu_mfcio.h>
#include "ppu_fiber_signal.h"

static volatile uint8_t s_line[128] __attribute__((aligned(128)));

int cellFiberPpuGetScheduler(uint32_t eaFiber, uint32_t *pEaScheduler)
{
	if (eaFiber == 0 || pEaScheduler == 0)
		return FIBER_ERROR_NULL_POINTER;
	if (eaFiber & 0x7f)
		return FIBER_ERROR_ALIGN;
	mfc_getllar(s_line, eaFiber, 0, 0);
	mfc_read_atomic_status();
	spu_dsync();
	*pEaScheduler = *(volatile uint32_t *)(s_line + 8);
	return 0;
}
