/* SPU job-queue runtime, part 2: submitting commands to a job queue from
 * SPU code (jobs, tasks) - push job, flush, sync.
 *
 * A job queue's first line (main memory) holds the command ring state:
 *   0x00 u16  read index (the policy module consumes from here)
 *   0x02 u16  write index (published commands end here)
 *   0x04 u8   commands being written (in flight)
 *   0x05 u8,  0x06 u8  pushers waiting for room
 *   0x07 u8   bit 5 = the write index's lap, bit 6 = the read index's lap
 *   0x10 u32  command ring EA (8 bytes per command)
 *   0x14 u16  ring depth, 0x16 u16  in flight + waiting limit
 *   0x6c u32  workload id, 0x7a u8  nonzero: single-writer ring
 * A command is a u64; bit 0 carries the lap it was written in, so the
 * policy module can tell a written slot from a stale one.  A pusher
 * reserves a slot among [write, write + in flight), writes its command
 * there, then publishes: write + 1 (wrapping flips the lap), in flight - 1.
 * Commands: job   high word = semaphore EA | ready count << 3 | 4 when
 *                 exclusive, low word = job EA | (descriptor size / 128) * 2
 *           flush high word = 3
 *           sync  high word = tag mask << 16 | 7
 * Independently written from the published object layouts and semantics.
 */
#include <stdint.h>
#include <spu_intrinsics.h>
#include <spu_mfcio.h>

#define JOB_AGAIN  0x80410A01u
#define JOB_INVAL  0x80410A02u
#define JOB_PERM   0x80410A09u
#define JOB_ALIGN  0x80410A10u
#define JOB_NULL   0x80410A11u

#define MAX_HANDLES 1024
#define JQ_BITMAP   0x580

#define CMD_FLUSH   3ull
#define CMD_SYNC    7ull
#define CMD_EXCL    4ull

extern int _cellSpursTaskCanCallBlockWait(void);
extern int _cellSpursSendWorkloadSignal(unsigned int wid);

static uint8_t line[128] __attribute__((aligned(128)));
static uint8_t ring[128] __attribute__((aligned(128)));

#define L8(o)  (*(volatile uint8_t *)(line + (o)))
#define L16(o) (*(volatile uint16_t *)(line + (o)))
#define L32(o) (*(volatile uint32_t *)(line + (o)))

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

static int handle_open(uint64_t jq, int handle)
{
	const unsigned h = (unsigned)handle;
	if (h >= MAX_HANDLES)
		return 0;
	get_line(line, jq + JQ_BITMAP);
	return (L32((h / 32) * 4) >> (31 - h % 32)) & 1;
}

/* A blocking push found the ring full: let the consumer run.  A task gives
   its SPU back (the job queue's policy module may need exactly that SPU to
   drain the ring, so spinning there can wait forever); anything else - a
   blocking push is only allowed from a task, but be safe - pauses briefly,
   counted in loop iterations since the decrementer need not be running. */
extern int cellSpursYield(void);

static void wait_for_room(void)
{
	volatile unsigned n;
	if (!_cellSpursTaskCanCallBlockWait()) {
		(void)cellSpursYield();
		return;
	}
	for (n = 0; n < 2000; ++n)
		;
}

/* single-writer ring (+0x7a != 0): reserve, write, publish in one go */
static int submit_single(uint64_t jq, uint64_t cmd, unsigned tag, int blocking, int *wasEmpty)
{
	static uint64_t slot[2] __attribute__((aligned(16)));
	unsigned index, lap;
	for (;;) {
		unsigned read, write, flags, depth;
		get_line(line, jq);
		read = L16(0x00);
		write = L16(0x02);
		flags = L8(0x07);
		depth = L16(0x14);
		if (read == write && ((flags >> 5) & 1) != ((flags >> 6) & 1)) {
			if (!blocking)
				return (int)JOB_AGAIN;
			wait_for_room();                      /* full: wait for the consumer */
			continue;
		}
		*wasEmpty = read == write;
		index = write;
		lap = (flags >> 5) & 1;
		if (write + 1 == depth) {
			L16(0x02) = 0;
			L8(0x07) = (uint8_t)(flags ^ 0x20);
		} else
			L16(0x02) = (uint16_t)(write + 1);
		if (put_line(line, jq))
			break;
	}
	slot[index & 1] = cmd | lap;
	mfc_put(&slot[index & 1], L32(0x10) + index * 8, 8, tag, 0, 0);
	mfc_write_tag_mask(1u << tag);
	(void)mfc_read_tag_status_all();
	return 0;
}

/* multi-writer ring (+0x7a == 0) */
static int submit_multi(uint64_t jq, uint64_t cmd, unsigned tag, int blocking, int *wasEmpty)
{
	unsigned depth, k;
	(void)tag;
	/* 1: reserve a place among the commands in flight */
	for (;;) {
		unsigned read, write, inflight, flags, eff, lap;
		get_line(line, jq);
		read = L16(0x00);
		write = L16(0x02);
		inflight = L8(0x04);
		flags = L8(0x07);
		depth = L16(0x14);
		if (inflight + L8(0x06) == L16(0x16)) {
			if (!blocking)
				return (int)JOB_AGAIN;
			wait_for_room();
			continue;
		}
		eff = write + inflight;
		lap = (flags >> 5) & 1;
		if (eff >= depth) {
			eff -= depth;
			lap ^= 1;
		}
		if (eff == read && lap != ((flags >> 6) & 1)) {
			if (!blocking)
				return (int)JOB_AGAIN;
			wait_for_room();
			continue;
		}
		L8(0x04) = (uint8_t)(inflight + 1);
		if (put_line(line, jq))
			break;
	}

	/* 2: claim a slot not yet written in this lap, from the write index on */
	for (k = 0;; ++k) {
		unsigned write, inflight, lapNow, slotIndex, lap, off;
		uint64_t ringEa, lineEa;
		get_line(line, jq);
		write = L16(0x02);
		inflight = L8(0x04);
		lapNow = (L8(0x07) >> 5) & 1;
		if (k >= inflight) {
			k = (unsigned)-1;               /* published meanwhile: rescan */
			continue;
		}
		slotIndex = write + k;
		lap = lapNow;
		if (slotIndex >= depth) {
			slotIndex -= depth;
			lap ^= 1;
		}
		ringEa = L32(0x10) + (uint64_t)slotIndex * 8;
		lineEa = ringEa & ~0x7full;
		off = (unsigned)(ringEa & 0x7f);
		get_line(ring, lineEa);
		if ((*(volatile uint32_t *)(ring + off + 4) & 1) == lap)
			continue;                       /* taken this lap */
		*(volatile uint64_t *)(ring + off) = cmd | lap;
		if (put_line(ring, lineEa))
			break;
		k = (unsigned)-1;                   /* lost the line: rescan */
	}

	/* 3: publish */
	do {
		unsigned read, write, flags;
		get_line(line, jq);
		read = L16(0x00);
		write = L16(0x02);
		flags = L8(0x07);
		*wasEmpty = read == write && ((flags >> 5) & 1) == ((flags >> 6) & 1);
		L8(0x04) = (uint8_t)(L8(0x04) - 1);
		if (write + 1 == L16(0x14)) {
			L16(0x02) = 0;
			L8(0x07) = (uint8_t)(flags ^ 0x20);
		} else
			L16(0x02) = (uint16_t)(write + 1);
	} while (!put_line(line, jq));
	return 0;
}

static int pm_submit(uint64_t jq, int handle, uint64_t cmd, unsigned tag, int blocking)
{
	int rc, wasEmpty = 0;
	if (!handle_open(jq, handle))
		return (int)JOB_INVAL;
	get_line(line, jq);
	if (L8(0x7a))
		rc = submit_single(jq, cmd, tag, blocking, &wasEmpty);
	else
		rc = submit_multi(jq, cmd, tag, blocking, &wasEmpty);
	if (rc)
		return rc;
	get_line(line, jq);
	(void)_cellSpursSendWorkloadSignal(L32(0x6c) & 0x1f);
	return 0;
}

static int check_queue(uint64_t jq)
{
	if (!jq)
		return (int)JOB_NULL;
	if (jq & 0x7f)
		return (int)JOB_ALIGN;
	return 0;
}

int _cellSpursJobQueuePushFlush(uint64_t eaJobQueue, int handle, unsigned int tag, unsigned int isBlocking)
{
	int rc = check_queue(eaJobQueue);
	if (rc)
		return rc;
	if (tag > 31)
		return (int)JOB_INVAL;
	if (isBlocking && _cellSpursTaskCanCallBlockWait())
		return (int)JOB_PERM;
	return pm_submit(eaJobQueue, handle, CMD_FLUSH << 32, tag, isBlocking);
}

int _cellSpursJobQueuePushSync(uint64_t eaJobQueue, int handle, unsigned int tagMask, unsigned int tag,
                               unsigned int isBlocking)
{
	int rc = check_queue(eaJobQueue);
	if (rc)
		return rc;
	if (!tagMask || (tagMask & 0xffff0000u) || tag > 31)
		return (int)JOB_INVAL;
	if (isBlocking && _cellSpursTaskCanCallBlockWait())
		return (int)JOB_PERM;
	return pm_submit(eaJobQueue, handle, ((uint64_t)tagMask << 48) | (CMD_SYNC << 32), tag, isBlocking);
}

int _cellSpursJobQueuePushJobBody(uint64_t eaJobQueue, int handle, uint64_t eaJob, unsigned int sizeDesc,
                                  unsigned int numReadyCount, unsigned int tag, uint64_t eaSemaphore,
                                  unsigned int isExclusive, unsigned int isBlocking)
{
	uint64_t cmd;
	if (!eaJobQueue || !eaJob)
		return (int)JOB_NULL;
	if ((eaJob & 15) || (eaJobQueue & 0x7f) || (eaSemaphore & 0x7f))
		return (int)JOB_ALIGN;
	if ((sizeDesc != 64 && (sizeDesc & 0x7f)) || sizeDesc > 1023 || numReadyCount > 15 || tag > 31)
		return (int)JOB_INVAL;
	if (isBlocking && _cellSpursTaskCanCallBlockWait())
		return (int)JOB_PERM;
	cmd = ((uint64_t)((uint32_t)eaSemaphore | numReadyCount << 3 | (isExclusive ? (unsigned)CMD_EXCL : 0)) << 32)
	    | (uint32_t)eaJob | (sizeDesc >> 7) * 2;
	return pm_submit(eaJobQueue, handle, cmd, tag, isBlocking);
}

/* ---- job lists, Job2 and pushes that release pool descriptors ----------- */

#define CMD_LIST     1u
#define CMD_RELEASE  2u
#define JQ_POOL_INFO 0x660          /* u32 pool start, u32 pool end */

int _cellSpursJobQueuePushJobListBody(uint64_t eaJobQueue, int handle, uint64_t eaJobList, unsigned int tag,
                                      unsigned int dmaTag, uint64_t eaSemaphore, unsigned int isBlocking)
{
	uint64_t cmd;
	if (!eaJobQueue || !eaJobList)
		return (int)JOB_NULL;
	if ((eaJobList & 15) || (eaJobQueue & 0x7f) || (eaSemaphore & 0x7f))
		return (int)JOB_ALIGN;
	if (tag > 15 || dmaTag > 31)
		return (int)JOB_INVAL;
	if (isBlocking && _cellSpursTaskCanCallBlockWait())
		return (int)JOB_PERM;
	cmd = ((uint64_t)((uint32_t)eaSemaphore | tag << 3 | CMD_LIST) << 32) | (uint32_t)eaJobList;
	return pm_submit(eaJobQueue, handle, cmd, dmaTag, isBlocking);
}

/* is the descriptor inside the job queue's descriptor pool */
static int in_pool(uint64_t jq, uint64_t eaJob)
{
	static uint32_t pool[4] __attribute__((aligned(16)));
	mfc_get(pool, jq + JQ_POOL_INFO, 16, 0, 0, 0);
	mfc_write_tag_mask(1u << 0);
	(void)mfc_read_tag_status_all();
	return (uint32_t)eaJob >= pool[0] && (uint32_t)eaJob < pool[1];
}

/* flag: bit 1 exclusive, bit 2 do not block */
static int push_job2(uint64_t jq, int handle, uint64_t eaJob, unsigned sizeDesc, unsigned tag, unsigned dmaTag,
                     unsigned flag, uint64_t eaSemaphore, int release)
{
	const int blocking = !(flag & 4);
	uint64_t cmd;
	if (flag & ~7u)
		return (int)JOB_INVAL;
	if (!jq || !eaJob)
		return (int)JOB_NULL;
	if ((eaJob & 15) || (jq & 0x7f) || (eaSemaphore & 0x7f))
		return (int)JOB_ALIGN;
	if ((sizeDesc != 64 && (sizeDesc & 0x7f)) || sizeDesc > 1023 || tag > 15 || dmaTag > 31)
		return (int)JOB_INVAL;
	if (blocking && _cellSpursTaskCanCallBlockWait())
		return (int)JOB_PERM;
	if (in_pool(jq, eaJob) != release)
		return (int)JOB_INVAL;      /* pool descriptors go back with PushAndRelease only */
	cmd = ((uint64_t)((uint32_t)eaSemaphore | tag << 3 | ((flag >> 1) & 1 ? (unsigned)CMD_EXCL : 0)
	                  | (release ? CMD_RELEASE : 0)) << 32)
	    | (uint32_t)eaJob | (sizeDesc >> 7) * 2;
	return pm_submit(jq, handle, cmd, dmaTag, blocking);
}

int _cellSpursJobQueuePushJob2Body(uint64_t eaJobQueue, int handle, uint64_t eaJob, unsigned int sizeDesc,
                                   unsigned int tag, unsigned int dmaTag, unsigned int flag, uint64_t eaSemaphore)
{
	return push_job2(eaJobQueue, handle, eaJob, sizeDesc, tag, dmaTag, flag, eaSemaphore, 0);
}

int _cellSpursJobQueuePushAndReleaseJobBody(uint64_t eaJobQueue, int handle, uint64_t eaJob, unsigned int sizeDesc,
                                            unsigned int tag, unsigned int dmaTag, unsigned int flag,
                                            uint64_t eaSemaphore)
{
	return push_job2(eaJobQueue, handle, eaJob, sizeDesc, tag, dmaTag, flag, eaSemaphore, 1);
}
