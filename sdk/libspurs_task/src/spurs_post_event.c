/* SPU side of the SPURS event multiplexer: cellSpursPostEvent and
 * cellSpursEventMultiplexerPostEvent.
 *
 * An event multiplexer is a 128-byte line in main memory:
 *   0x00 u32  pending  nonzero while posted events wait for the PPU
 *   0x04 u32  port     SPU thread event port of the PPU handler
 *   0x08 u32  data     data0 of the event thrown to that port
 *   0x18 u64  head     EA of the most recently posted event record
 * An event record is 16 bytes: u64 next (the previous head), u64 data.
 * Posting links the record in at the head; the post that finds the
 * multiplexer idle throws one event to the handler's port.  A CellSpurs
 * instance carries its multiplexer at +0xf00.
 * Independently written from the published object layouts and semantics.
 */
#include <stdint.h>
#include <spu_mfcio.h>
#include <sys/spu_event.h>
#include "spurs_line.h"

#define CORE_ALIGN 0x80410710u
#define CORE_NULL  0x80410711u
#define SPURS_EVENT_MULTIPLEXER 0xf00

static uint32_t line[32] __attribute__((aligned(128)));
static uint64_t record[2] __attribute__((aligned(16)));

int cellSpursEventMultiplexerPostEvent(uint64_t eaMultiplexer, uint64_t eaEvent, uint64_t data)
{
	const unsigned tag = spurs_kernel_tag();
	uint32_t pending;
	if (!eaMultiplexer || !eaEvent)
		return (int)CORE_NULL;
	if ((eaMultiplexer & 0x7f) || (eaEvent & 0x7f))
		return (int)CORE_ALIGN;
	do {
		line_get_at(line, eaMultiplexer);
		pending = LINE_U32(line, 0x00);
		LINE_U32(line, 0x00) = pending | 1;
		record[0] = LINE_U64(line, 0x18);
		record[1] = data;
		LINE_U64(line, 0x18) = eaEvent;
		dma_put_wait(record, eaEvent, sizeof record, tag);
	} while (!line_put_at(line, eaMultiplexer));
	if (!pending)
		(void)sys_spu_thread_throw_event((uint8_t)LINE_U32(line, 0x04), LINE_U32(line, 0x08), 0);
	return 0;
}

int cellSpursPostEvent(uint64_t eaSpurs, uint64_t eaEvent, uint64_t data)
{
	if (!eaSpurs || !eaEvent)
		return (int)CORE_NULL;
	if ((eaSpurs & 0x7f) || (eaEvent & 0x7f))
		return (int)CORE_ALIGN;
	return cellSpursEventMultiplexerPostEvent(eaSpurs + SPURS_EVENT_MULTIPLEXER, eaEvent, data);
}
