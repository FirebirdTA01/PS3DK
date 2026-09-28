/* SPU job-queue runtime: sleeping until the job queue's policy module
 * wakes the caller.
 *
 * A task that must wait for the job queue - for room in its command ring,
 * or for a free descriptor in its pool - registers in a waiter ring and
 * sleeps in cellSpursWaitSignal; the policy module signals the oldest
 * waiter when it frees what they wait for.  Sleeping, not spinning, is what
 * lets the job queue run: it may be allowed only on the SPU the waiting
 * task holds.
 *
 * A waiter ring has a record in a job-queue line (at recOff):
 *   +0 u16 read index (the policy module's), +2 u16 write index,
 *   +4 u8  entries being written, +6 u8 the write index's lap
 * and `slots` u64 entries in main memory, each
 *   (taskset EA | task id) << 32 | 0x01000000 | lap
 * The policy module sets bit 1 of an entry it wakes before the entry is
 * written, so a waiter that finds it set does not sleep.
 * Independently written from the published object layouts and semantics.
 */
#include <stdint.h>
#include <spu_mfcio.h>

extern uint64_t cellSpursGetTasksetAddress(void);
extern unsigned int cellSpursGetTaskId(void);
extern int cellSpursWaitSignal(void);

static uint8_t rline[128] __attribute__((aligned(128)));
static uint8_t eline[128] __attribute__((aligned(128)));
static uint8_t scratch[128] __attribute__((aligned(128)));

#define W8(o)  (*(volatile uint8_t *)(rline + (o)))
#define W16(o) (*(volatile uint16_t *)(rline + (o)))

static inline void get_line(void *ls, uint64_t ea)
{
	mfc_getllar(ls, ea, 0, 0);
	(void)mfc_read_atomic_status();
	spu_dsync();
}

static inline int put_line(void *ls, uint64_t ea)
{
	spu_dsync();
	mfc_putllc(ls, ea, 0, 0);
	return (mfc_read_atomic_status() & MFC_PUTLLC_STATUS) == 0;
}

static uint64_t read_entry(uint64_t ringEa, unsigned slot)
{
	const uint64_t ea = ringEa + slot * 8u;
	get_line(scratch, ea & ~0x7full);
	return *(volatile uint64_t *)(scratch + (ea & 0x7f));
}

/* the lap an entry at slot carries once written, seen from write index w */
static inline unsigned slot_lap(unsigned slot, unsigned w, unsigned lapW)
{
	return slot < w ? lapW ^ 1 : lapW;
}

/* Register in the waiter ring whose record is at lineEa + recOff and sleep
   until woken.  Returns 0, or the error of cellSpursWaitSignal. */
int _cellSpursJobQueueWaitInRing(uint64_t lineEa, unsigned recOff, uint64_t ringEa, unsigned slots)
{
	const unsigned b = recOff;
	const uint64_t me = (uint64_t)((uint32_t)cellSpursGetTasksetAddress() | cellSpursGetTaskId()) << 32
	                  | 0x01000000u;
	uint64_t old;
	unsigned k = 0;

	do {
		get_line(rline, lineEa);
		if (W8(b + 4) > 126)
			spu_stop(0);
		W8(b + 4) = (uint8_t)(W8(b + 4) + 1);
	} while (!put_line(rline, lineEa));

	for (;;) {
		unsigned w, lapW, inflight, slot, lap, i;
		uint64_t ea, entryLine;
		get_line(rline, lineEa);
		w = W16(b + 2);
		lapW = W8(b + 6) & 1;
		inflight = W8(b + 4);
		if (k >= inflight) {
			k = 0;                          /* published meanwhile: rescan */
			continue;
		}
		slot = (w + k) % slots;
		/* entries before ours must be written first */
		for (i = 0; i < k; ++i) {
			const unsigned s = (w + i) % slots;
			if ((read_entry(ringEa, s) & 1) != slot_lap(s, w, lapW))
				break;
		}
		if (i < k) {
			k = 0;
			continue;
		}
		lap = slot_lap(slot, w, lapW);
		ea = ringEa + slot * 8u;
		entryLine = ea & ~0x7full;
		get_line(eline, entryLine);
		old = *(volatile uint64_t *)(eline + (ea & 0x7f));
		if ((old & 1) == lap) {
			++k;                            /* taken in this lap */
			continue;
		}
		*(volatile uint64_t *)(eline + (ea & 0x7f)) = me | lap;
		if (put_line(eline, entryLine))
			break;
	}

	/* publish */
	do {
		unsigned w;
		get_line(rline, lineEa);
		W8(b + 4) = (uint8_t)(W8(b + 4) - 1);
		w = W16(b + 2) + 1u;
		if (w == slots) {
			w = 0;
			W8(b + 6) = W8(b + 6) ? 0 : 1;
		}
		W16(b + 2) = (uint16_t)w;
	} while (!put_line(rline, lineEa));

	if (old & 2)
		return 0;                       /* woken before we slept */
	return cellSpursWaitSignal();
}
