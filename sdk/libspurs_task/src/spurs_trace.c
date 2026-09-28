/* SPU side of SPURS tracing: cellSpursModulePutTrace, cellSpursPutTrace,
 * cellSpursPutUserTrace.
 *
 * The kernel keeps, in local storage:
 *   0x100..0x17f  a copy of the CellSpurs instance's first line; byte
 *                 0x175 is 1 while tracing is on
 *   0x1c8 u32     this SPU's number, 0x1dc u32 the running workload id
 *   0x210         the trace buffer cursor: u32 EA high, u32 EA low (bit 0
 *                 = wrap around), u32 next packet index, u32 packet count
 * A put stamps the packet header (length 2, SPU, workload, the negated
 * decrementer as the time), advances the cursor and DMAs the 16-byte
 * packet to the buffer under the caller's tag without waiting for it.
 * cellSpursPutTrace first refreshes the instance copy, so it sees tracing
 * switched on or off by the PPU.
 * Independently written from the published object layouts and semantics.
 */
#include <stdint.h>
#include <spu_intrinsics.h>
#include <spu_mfcio.h>
#include <cell/spurs/trace.h>

#define LS8(a)  (*(volatile uint8_t *)(uintptr_t)(a))
#define LS32(a) (*(volatile uint32_t *)(uintptr_t)(a))

static void put_trace(void *packet, unsigned tag)
{
	volatile uint32_t *const cursor = &LS32(0x210);
	uint32_t index, next;
	uint8_t *p = (uint8_t *)packet;
	if (LS8(0x175) != 1)
		return;
	index = cursor[2];
	if (index >= cursor[3] || !((cursor[1] >> 4) | cursor[0]))
		return;
	next = index + 1;
	if (next == cursor[3] && (cursor[1] & 1))
		next = 0;                           /* wrap around */
	cursor[2] = next;

	p[1] = 2;
	p[2] = (uint8_t)LS32(0x1c8);
	p[3] = (uint8_t)LS32(0x1dc);
	*(uint32_t *)(p + 4) = 0u - spu_readch(SPU_RdDec);
	spu_dsync();
	mfc_put(packet, ((uint64_t)cursor[0] << 32) | (((cursor[1] >> 4) + index) << 4), 16, tag, 0, 0);
}

void cellSpursModulePutTrace(CellSpursTracePacket *packet, unsigned tag)
{
	put_trace(packet, tag);
}

void cellSpursPutUserTrace(CellTraceHeader *packet, unsigned tag)
{
	put_trace(packet, tag);
}

void cellSpursPutTrace(CellSpursTracePacket *packet, unsigned tag)
{
	/* refresh the kernel's copy of the instance's first line */
	mfc_getllar((volatile void *)(uintptr_t)0x100, *(volatile uint64_t *)(uintptr_t)0x1c0, 0, 0);
	(void)mfc_read_atomic_status();
	spu_dsync();
	put_trace(packet, tag);
}
