/* SPU-side SPURS instance controls: workload ready counts, idle-SPU
 * requests, workload data, priorities and contention, plus the SPU-side
 * semaphore initializer and its taskset query.
 *
 * CellSpurs fields used (main memory, from the instance EA):
 *   0x00 u8[16]  ready count of workloads 0..15
 *   0x10 u8[16]  ready count of workloads 16..31 (32-workload instances),
 *                otherwise the idle-SPU request of workloads 0..15
 *   0x50 u8[16]  max contention: low nibble workload n, high nibble n+16
 *   0x72 u8      system-service message: one bit per SPU (MSB = SPU 0)
 *   0x74 u8      flags; 0x40 = the instance has 32 workloads
 *   0x76 u8      SPU count
 *   0xbd u8      "update workload" message: one bit per SPU
 *   0xb00, 0x1000  0x20-byte records of workloads 0..15 / 16..31:
 *                +0x08 u64 argument, +0x18 u8[8] priority per SPU
 * Independently written from the published object layouts and semantics.
 */
#include <stdint.h>
#include <spu_mfcio.h>
#include "spurs_line.h"

#define CORE_INVAL  0x80410702u
#define PM_INVAL    0x80410802u
#define PM_STAT     0x8041080Fu
#define SEM_NULL    0x80410911u
#define SEM_ALIGN   0x80410910u

#define FLAG_32_WORKLOADS 0x40

extern uint64_t cellSpursGetSpursAddress(void);
extern uint64_t cellSpursGetTasksetAddress(void);

static uint8_t line[128] __attribute__((aligned(128)));
static uint64_t scratch[4] __attribute__((aligned(128)));

enum { OP_SWAP, OP_CAS, OP_ADD };

static int wide(void)
{
	static uint8_t first[128] __attribute__((aligned(128)));
	line_get_at(first, cellSpursGetSpursAddress());
	return (first[0x74] & FLAG_32_WORKLOADS) != 0;
}

/* the instance line, read once (no reservation kept) */
static void spurs_line(void)
{
	line_get_at(line, cellSpursGetSpursAddress());
}

static uint64_t workload_record(unsigned wid)
{
	return cellSpursGetSpursAddress() + (wid < 16 ? 0xb00 : 0x1000) + (uint64_t)(wid & 15) * 0x20;
}

unsigned _cellSpursReadyCountOperator(unsigned int wid, unsigned int op, unsigned int value1,
                                      unsigned int value2)
{
	const uint64_t spurs = cellSpursGetSpursAddress();
	const unsigned off = (wid > 15 ? 0x10 : 0x00) + (wid & 15);
	unsigned old;
	do {
		int next;
		line_get_at(line, spurs);
		old = line[off];
		switch (op) {
		case OP_SWAP:
			next = (int)value1;
			break;
		case OP_CAS:
			if (old != value1)
				return old;
			next = (int)value2;
			break;
		case OP_ADD:
			next = (int)old + (int)value1;
			next = next > 255 ? 255 : next < 0 ? 0 : next;
			break;
		default:
			next = (int)old;
			break;
		}
		line[off] = (uint8_t)next;
	} while (!line_put_at(line, spurs));
	return old;
}

unsigned _cellSpursReadyCountSwap(unsigned int wid, unsigned int value)
{
	return _cellSpursReadyCountOperator(wid, OP_SWAP, value, 0);
}

unsigned _cellSpursReadyCountCompareAndSwap(unsigned int wid, unsigned int compare, unsigned int swap)
{
	return _cellSpursReadyCountOperator(wid, OP_CAS, compare, swap);
}

unsigned _cellSpursReadyCountAdd(unsigned int wid, int value)
{
	return _cellSpursReadyCountOperator(wid, OP_ADD, (unsigned)value, 0);
}

/* the argument the workload was added with (0 for a workload the
 * instance cannot have) */
uint64_t _cellSpursGetWorkloadData(unsigned int wid)
{
	const unsigned tag = spurs_kernel_tag();
	if (wid > 15 && !wide())
		return 0;
	dma_get_wait(scratch, workload_record(wid), 0x20, tag);
	return scratch[1];
}

/* 16-workload instances only: ask for `count` idle SPUs for workload `wid` */
int _cellSpursRequestIdleSpu(unsigned int wid, unsigned int count)
{
	const uint64_t spurs = cellSpursGetSpursAddress();
	if (wide())
		return (int)PM_STAT;
	if (count > 8 || wid > 15)
		return (int)PM_INVAL;
	do {
		line_get_at(line, spurs);
		line[0x10 + wid] = (uint8_t)count;
	} while (!line_put_at(line, spurs));
	return 0;
}

int cellSpursSetMaxContention(unsigned int wid, unsigned int maxContention)
{
	const uint64_t spurs = cellSpursGetSpursAddress();
	const unsigned off = 0x50 + (wid & 15);
	const uint8_t keep = wid > 15 ? 0x0f : 0xf0;
	uint8_t v;
	if (wid >= (wide() ? 32u : 16u))
		return (int)CORE_INVAL;
	if (maxContention > 8)
		maxContention = 8;
	v = (uint8_t)(maxContention | maxContention << 4);
	do {
		line_get_at(line, spurs);
		line[off] = (uint8_t)((line[off] & keep) | (v & (uint8_t)~keep));
	} while (!line_put_at(line, spurs));
	return 0;
}

/* tell the SPUs in `spuMask` (MSB = SPU 0) to reread workload priorities */
static void notify_priority(uint8_t spuMask)
{
	const uint64_t spurs = cellSpursGetSpursAddress();
	do {
		line_get_at(line, spurs + 0x80);
		line[0xbd - 0x80] |= spuMask;
	} while (!line_put_at(line, spurs + 0x80));
	do {
		line_get_at(line, spurs);
		line[0x72] |= spuMask;
	} while (!line_put_at(line, spurs));
}

int cellSpursSetPriority(unsigned int wid, unsigned int spu, unsigned int priority)
{
	static uint8_t byte[16] __attribute__((aligned(16)));
	const unsigned tag = spurs_kernel_tag();
	const uint64_t ea = workload_record(wid) + 0x18 + spu;
	spurs_line();
	if (wid >= (wide() ? 32u : 16u) || spu >= line[0x76] || priority > 15)
		return (int)CORE_INVAL;
	byte[ea & 15] = (uint8_t)priority;
	dma_put_wait(&byte[ea & 15], ea, 1, tag);
	notify_priority((uint8_t)(0x80u >> spu));
	return 0;
}

int cellSpursSetPriorities(unsigned int wid, vec_uchar16 priorities)
{
	const unsigned tag = spurs_kernel_tag();
	unsigned i;
	if (wid >= (wide() ? 32u : 16u))
		return (int)CORE_INVAL;
	for (i = 0; i < 8; ++i)
		if (spu_extract(priorities, i) > 15)
			return (int)CORE_INVAL;
	for (i = 0; i < 8; ++i)
		((uint8_t *)&scratch[3])[i] = spu_extract(priorities, i);
	dma_put_wait(&scratch[3], workload_record(wid) + 0x18, 8, tag);
	notify_priority(0xff);
	return 0;
}

/* ---- semaphore: +0x00 s32 count, +0x30 u64 SPURS EA, +0x38 u64 taskset
 * EA (0 for an IWL semaphore) ------------------------------------------ */

int _cellSpursSemaphoreInitialize(uint64_t ea, int total, unsigned int isIWL)
{
	unsigned i;
	if (!ea)
		return (int)SEM_NULL;
	if (ea & 0x7f)
		return (int)SEM_ALIGN;
	for (i = 0; i < sizeof line; ++i)
		line[i] = 0;
	LINE_U32(line, 0x00) = (uint32_t)total;
	LINE_U64(line, 0x30) = cellSpursGetSpursAddress();
	LINE_U64(line, 0x38) = isIWL ? 0 : cellSpursGetTasksetAddress();
	spu_dsync();
	mfc_putlluc(line, ea, 0, 0);
	(void)mfc_read_atomic_status();
	return 0;
}

int cellSpursSemaphoreGetTasksetAddress(uint64_t ea, uint64_t *pEaTaskset)
{
	if (!ea || !pEaTaskset)
		return (int)SEM_NULL;
	if (ea & 0x7f)
		return (int)SEM_ALIGN;
	line_get_at(line, ea);
	*pEaTaskset = LINE_U64(line, 0x38);
	return 0;
}
