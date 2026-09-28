/* SPU job-queue runtime, part 5: taking a job descriptor from a job
 * queue's descriptor pool (_cellSpursJobQueueAllocateJobDescriptor).
 *
 * The pool has one size class per descriptor size: 64 bytes (class 0)
 * and 128 * n bytes (class n, 1..7).  Job-queue fields:
 *   0x600 u32[8]  descriptors per class (0: the class has no pool)
 *   0x640 u32[8]  EA of the class's waiter ring (128 u64 entries)
 *   0x680         one line, 16 bytes per class:
 *     +0  u32  free-list head; a free descriptor's first word links the next
 *     +4  u16  free descriptors
 *     +6  u8   waiters woken but not yet retried
 *     +7  u8   waiters
 *     +8  u16  waiter ring read index (the releasing side)
 *     +10 u16  waiter ring write index
 *     +12 u8   waiter entries being written
 *     +14 u8   the write index's lap
 * A descriptor pushed with PushAndRelease goes back on the free list when
 * its job finishes, and the releasing side wakes the oldest waiter.
 * A waiter entry is (taskset EA | task id) << 32 | 0x01000000 | lap; the
 * releasing side sets bit 1 of an entry it wakes before it is written.
 * Independently written from the published object layouts and semantics.
 */
#include <stddef.h>
#include <stdint.h>
#include <spu_intrinsics.h>
#include <spu_mfcio.h>

#define JOB_AGAIN  0x80410A01u
#define JOB_INVAL  0x80410A02u
#define JOB_PERM   0x80410A09u
#define JOB_ALIGN  0x80410A10u
#define JOB_NULL   0x80410A11u
#define TASK_WAIT_FAILED 0x80410914u

#define JQ_BITMAP    0x580
#define JQ_POOL_TAB  0x600
#define JQ_POOL_LINE 0x680
#define RING_SLOTS   128

extern int _cellSpursTaskCanCallBlockWait(void);
extern uint64_t cellSpursGetTasksetAddress(void);
extern unsigned int cellSpursGetTaskId(void);
extern int cellSpursWaitSignal(void);

static uint8_t pool[128] __attribute__((aligned(128)));
static uint8_t ringLine[128] __attribute__((aligned(128)));
static uint8_t scratch[128] __attribute__((aligned(128)));
static uint32_t tab[32] __attribute__((aligned(128)));
static uint32_t link[4] __attribute__((aligned(128)));

#define R8(r, o)  (*(volatile uint8_t *)(pool + (r) + (o)))
#define R16(r, o) (*(volatile uint16_t *)(pool + (r) + (o)))
#define R32(r, o) (*(volatile uint32_t *)(pool + (r) + (o)))

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

static void dma_get(void *ls, uint64_t ea, unsigned size, unsigned tag)
{
	mfc_get(ls, ea, size, tag, 0, 0);
	mfc_write_tag_mask(1u << tag);
	(void)mfc_read_tag_status_all();
}

static int handle_open(uint64_t jq, int handle)
{
	const unsigned h = (unsigned)handle;
	if (h >= 1024)
		return 0;
	get_line(scratch, jq + JQ_BITMAP);
	return (*(volatile uint32_t *)(scratch + (h / 32) * 4) >> (31 - h % 32)) & 1;
}

/* the lap an entry at slot carries once written, seen from write index w */
static inline unsigned slot_lap(unsigned slot, unsigned w, unsigned lapW)
{
	return slot < w ? lapW ^ 1 : lapW;
}

static uint64_t read_entry(uint64_t ringEa, unsigned slot)
{
	const uint64_t ea = ringEa + slot * 8u;
	get_line(scratch, ea & ~0x7full);
	return *(volatile uint64_t *)(scratch + (ea & 0x7f));
}

/* put ourselves in the class's waiter ring; returns the entry's old value */
static uint64_t enter_ring(uint64_t jq, unsigned r, uint64_t ringEa, uint64_t me)
{
	uint64_t old;
	unsigned k = 0;

	do {
		get_line(pool, jq + JQ_POOL_LINE);
		if (R8(r, 12) > 126)
			spu_stop(0);
		R8(r, 12) = (uint8_t)(R8(r, 12) + 1);
	} while (!put_line(pool, jq + JQ_POOL_LINE));

	for (;;) {
		unsigned w, lapW, inflight, slot, gap, lap, i;
		uint64_t ea, lineEa;
		get_line(pool, jq + JQ_POOL_LINE);
		w = R16(r, 10);
		lapW = R8(r, 14) & 1;
		inflight = R8(r, 12);
		slot = (w + k) % RING_SLOTS;
		gap = k;
		if (gap >= inflight) {
			k = 0;                          /* published meanwhile: rescan */
			continue;
		}
		/* everything before our slot must be written first */
		for (i = 0; i < gap; ++i) {
			const unsigned s = (w + i) % RING_SLOTS;
			if ((read_entry(ringEa, s) & 1) != slot_lap(s, w, lapW))
				break;
		}
		if (i < gap) {
			k = 0;
			continue;
		}
		lap = slot_lap(slot, w, lapW);
		ea = ringEa + slot * 8u;
		lineEa = ea & ~0x7full;
		get_line(ringLine, lineEa);
		old = *(volatile uint64_t *)(ringLine + (ea & 0x7f));
		if ((old & 1) == lap) {
			++k;                            /* taken in this lap */
			continue;
		}
		*(volatile uint64_t *)(ringLine + (ea & 0x7f)) = me | lap;
		if (put_line(ringLine, lineEa))
			break;
	}

	/* publish */
	do {
		unsigned w;
		get_line(pool, jq + JQ_POOL_LINE);
		R8(r, 12) = (uint8_t)(R8(r, 12) - 1);
		w = R16(r, 10) + 1u;
		if (w == RING_SLOTS) {
			w = 0;
			R8(r, 14) = R8(r, 14) ? 0 : 1;
		}
		R16(r, 10) = (uint16_t)w;
	} while (!put_line(pool, jq + JQ_POOL_LINE));
	return old;
}

int _cellSpursJobQueueAllocateJobDescriptor(uint64_t eaJobQueue, int handle, size_t sizeJobDesc,
                                            unsigned int dmaTag, unsigned int flag,
                                            uint64_t *eaAllocatedJobDesc)
{
	const int blocking = !(flag & 4);
	unsigned n, r, woke = 0;
	uint64_t ringEa, me = 0;

	if (!eaJobQueue || !eaAllocatedJobDesc)
		return (int)JOB_NULL;
	if (!handle_open(eaJobQueue, handle))
		return (int)JOB_INVAL;
	if (eaJobQueue & 0x7f)
		return (int)JOB_ALIGN;
	if (flag & ~4u)
		return (int)JOB_INVAL;
	if (blocking && _cellSpursTaskCanCallBlockWait())
		return (int)JOB_PERM;
	if (sizeJobDesc != 64 && ((sizeJobDesc & 0x7f) || sizeJobDesc < 128))
		return (int)JOB_INVAL;
	if (sizeJobDesc > 1023)
		return (int)JOB_INVAL;
	n = (unsigned)sizeJobDesc >> 7;
	r = n * 16;
	dma_get(tab, eaJobQueue + JQ_POOL_TAB, 0x70, dmaTag);
	if (!tab[n])
		return (int)JOB_NULL;               /* no pool for this size */
	ringEa = tab[16 + n];

	for (;;) {
		uint32_t got;
		do {
			get_line(pool, eaJobQueue + JQ_POOL_LINE);
			if (woke) {
				R8(r, 7) = (uint8_t)(R8(r, 7) - 1);
				R8(r, 6) = (uint8_t)(R8(r, 6) - 1);
			}
			if (R8(r, 7) == 127)
				return (int)JOB_AGAIN;
			got = 0;
			if (R16(r, 4)) {
				got = R32(r, 0);
				dma_get(link, got, 4, dmaTag);
				R32(r, 0) = link[0];
				R16(r, 4) = (uint16_t)(R16(r, 4) - 1);
			} else {
				if (!blocking)
					return (int)JOB_AGAIN;
				R8(r, 7) = (uint8_t)(R8(r, 7) + 1);
			}
		} while (!put_line(pool, eaJobQueue + JQ_POOL_LINE));
		if (got) {
			*eaAllocatedJobDesc = got;
			return 0;
		}
		if (!me)
			me = (uint64_t)((uint32_t)cellSpursGetTasksetAddress() | cellSpursGetTaskId()) << 32 | 0x01000000u;
		if (!(enter_ring(eaJobQueue, r, ringEa, me) & 2) && cellSpursWaitSignal())
			return (int)TASK_WAIT_FAILED;
		woke = 1;
	}
}
