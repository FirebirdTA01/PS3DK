/* SPU job-queue runtime, part 4: job-queue ports, second revision
 * (CellSpursJobQueuePort2).
 *
 * Same idea as the first port: a 128-byte line holding a job-queue
 * handle, whose first 16 bytes are a job-queue semaphore that the jobs
 * pushed with CELL_SPURS_JOBQUEUE_FLAG_SYNC_JOB release.  The calls take
 * a flag word instead of booleans:
 *   bit 0  count the job for Port2Sync (sync job)
 *   bit 1  exclusive job
 *   bit 2  do not block
 * Line layout:
 *   0x00      job-queue semaphore
 *   0x10 u32  jobs pushed with sync since the last Port2Sync
 *   0x14 u8   a Port2Sync is running
 *   0x18 u32  job queue EA, 0x1c u32 job-queue handle
 *   0x20 u8   created, 0x21 u8 being created / destroyed
 * CopyPush takes its descriptor from the job queue's pool and pushes it
 * with release, so it can wait for a free descriptor: tasks only.
 * Independently written from the published object layouts and semantics.
 */
#include <stddef.h>
#include <stdint.h>
#include <spu_mfcio.h>

#define JOB_INVAL  0x80410A02u
#define JOB_PERM   0x80410A09u
#define JOB_BUSY   0x80410A0Au
#define JOB_STAT   0x80410A0Fu
#define JOB_ALIGN  0x80410A10u
#define JOB_NULL   0x80410A11u

#define FLAG_SYNC      1u
#define FLAG_NONBLOCK  4u
#define MAX_COUNT      0x0fffffffu

extern int cellSpursJobQueueOpen(uint64_t eaJobQueue, int *handle);
extern int cellSpursJobQueueClose(uint64_t eaJobQueue, int handle);
extern int cellSpursJobQueueSemaphoreInitialize(uint32_t eaSemaphore, uint32_t eaJobQueue);
extern int cellSpursJobQueueSemaphoreAcquire(uint32_t eaSemaphore, unsigned int count);
extern int cellSpursJobQueueSemaphoreTryAcquire(uint32_t eaSemaphore, unsigned int count);
extern int _cellSpursTaskCanCallBlockWait(void);
extern int _cellSpursJobQueuePushJobListBody(uint64_t eaJobQueue, int handle, uint64_t eaJobList, unsigned int tag,
                                             unsigned int dmaTag, uint64_t eaSemaphore, unsigned int isBlocking);
extern int _cellSpursJobQueuePushFlush(uint64_t eaJobQueue, int handle, unsigned int dmaTag, unsigned int isBlocking);
extern int _cellSpursJobQueuePushSync(uint64_t eaJobQueue, int handle, unsigned int tagMask, unsigned int dmaTag,
                                      unsigned int isBlocking);
extern int _cellSpursJobQueuePushJob2Body(uint64_t eaJobQueue, int handle, uint64_t eaJob, unsigned int sizeDesc,
                                          unsigned int tag, unsigned int dmaTag, unsigned int flag,
                                          uint64_t eaSemaphore);
extern int _cellSpursJobQueueAllocateJobDescriptor(uint64_t eaJobQueue, int handle, size_t sizeJobDesc,
                                                   unsigned int dmaTag, unsigned int flag,
                                                   uint64_t *eaAllocatedJobDesc);
extern int _cellSpursJobQueuePushAndReleaseJobBody(uint64_t eaJobQueue, int handle, uint64_t eaJob,
                                                   unsigned int sizeDesc, unsigned int tag, unsigned int dmaTag,
                                                   unsigned int flag, uint64_t eaSemaphore);

static uint8_t line[128] __attribute__((aligned(128)));

#define P8(o)  (*(volatile uint8_t *)(line + (o)))
#define P32(o) (*(volatile uint32_t *)(line + (o)))

static inline void get_line(uint64_t ea)
{
	mfc_getllar(line, ea, 0, 0);
	(void)mfc_read_atomic_status();
	spu_dsync();
}

static inline int put_line(uint64_t ea)
{
	spu_dsync();
	mfc_putllc(line, ea, 0, 0);
	return (mfc_read_atomic_status() & MFC_PUTLLC_STATUS) == 0;
}

static int check_port(uint64_t port)
{
	if (!port)
		return (int)JOB_NULL;
	if (port & 0x7f)
		return (int)JOB_ALIGN;
	return 0;
}

uint64_t cellSpursJobQueuePort2GetJobQueue(uint64_t eaPort2)
{
	get_line(eaPort2);
	return P32(0x18);
}

int cellSpursJobQueuePort2Create(uint64_t eaPort2, uint64_t eaJobQueue)
{
	int handle, rc;
	if (!eaPort2 || !eaJobQueue)
		return (int)JOB_NULL;
	if ((eaPort2 & 0x7f) || (eaJobQueue & 0x7f))
		return (int)JOB_ALIGN;
	do {
		get_line(eaPort2);
		for (unsigned i = 0; i < 0x40; i += 4)
			P32(i) = 0;
		P8(0x21) = 1;
	} while (!put_line(eaPort2));
	rc = cellSpursJobQueueOpen(eaJobQueue, &handle);
	if (rc)
		return rc;
	rc = cellSpursJobQueueSemaphoreInitialize((uint32_t)eaPort2, (uint32_t)eaJobQueue);
	if (rc) {
		cellSpursJobQueueClose(eaJobQueue, handle);
		return rc;
	}
	do {
		get_line(eaPort2);
		P32(0x18) = (uint32_t)eaJobQueue;
		P32(0x1c) = (uint32_t)handle;
		P8(0x20) = 1;
		P8(0x21) = 0;
	} while (!put_line(eaPort2));
	return 0;
}

int cellSpursJobQueuePort2Destroy(uint64_t eaPort2)
{
	uint32_t jq;
	int handle;
	int rc = check_port(eaPort2);
	if (rc)
		return rc;
	do {
		get_line(eaPort2);
		if (!P8(0x20) || P8(0x21) || !P32(0x18))
			return (int)JOB_STAT;
		if (P32(0x10))
			return (int)JOB_BUSY;          /* sync jobs not waited for */
		P8(0x21) = 1;
	} while (!put_line(eaPort2));
	jq = P32(0x18);
	handle = (int)P32(0x1c);
	if (cellSpursJobQueueClose(jq, handle))
		spu_stop(0);                        /* the handle was ours: cannot fail */
	do {
		get_line(eaPort2);
		P32(0x18) = 0;
		P8(0x20) = 0;
		P8(0x21) = 0;
	} while (!put_line(eaPort2));
	return 0;
}

/* count jobs pushed with sync */
static int count_add(uint64_t port, uint32_t n)
{
	do {
		get_line(port);
		if (P8(0x14) || P32(0x10) + n > MAX_COUNT || P32(0x10) + n < n)
			return (int)JOB_BUSY;
		P32(0x10) += n;
	} while (!put_line(port));
	return 0;
}

static int count_sub(uint64_t port, uint32_t n)
{
	do {
		get_line(port);
		if (P8(0x14))
			return (int)JOB_BUSY;
		P32(0x10) -= n;
	} while (!put_line(port));
	return 0;
}

static int port2_push(uint64_t port, uint64_t eaJob, unsigned size, unsigned tag, unsigned dmaTag, unsigned flag,
                      int release)
{
	const int sync = flag & FLAG_SYNC;
	int rc;
	if (flag & ~7u)
		return (int)JOB_INVAL;
	if (!port || !eaJob)
		return (int)JOB_NULL;
	if ((port & 0x7f) || (eaJob & 15))
		return (int)JOB_ALIGN;
	if ((size != 64 && (size & 0x7f)) || tag > 15 || size > 1023 || dmaTag > 31)
		return (int)JOB_INVAL;
	if (sync && (rc = count_add(port, 1)) != 0)
		return rc;
	get_line(port);
	if (release)
		rc = _cellSpursJobQueuePushAndReleaseJobBody(P32(0x18), (int)P32(0x1c), eaJob, size, tag, dmaTag, flag,
		                                             sync ? port : 0);
	else
		rc = _cellSpursJobQueuePushJob2Body(P32(0x18), (int)P32(0x1c), eaJob, size, tag, dmaTag, flag,
		                                    sync ? port : 0);
	if (rc && sync) {
		int rc2 = count_sub(port, 1);
		if (rc2)
			return rc2;
	}
	return rc;
}

int _cellSpursJobQueuePort2PushJobBody(uint64_t eaPort2, uint64_t eaJob, size_t sizeDesc, unsigned tag,
                                       unsigned int dmaTag, unsigned flag, unsigned isAutoRelease)
{
	return port2_push(eaPort2, eaJob, sizeDesc, tag, dmaTag, flag, isAutoRelease != 0);
}

int _cellSpursJobQueuePort2PushJobListBody(uint64_t eaPort2, uint64_t eaJobList, unsigned tag, unsigned int dmaTag,
                                           unsigned flag)
{
	static uint32_t list[4] __attribute__((aligned(16)));   /* numJobs, sizeOfJob, eaJobList */
	const int sync = flag & FLAG_SYNC;
	uint32_t n, size;
	int rc;
	if (flag & ~5u)
		return (int)JOB_INVAL;
	if (!eaPort2 || !eaJobList)
		return (int)JOB_NULL;
	if ((eaPort2 & 0x7f) || (eaJobList & 15))
		return (int)JOB_ALIGN;
	if (tag > 15 || dmaTag > 31)
		return (int)JOB_INVAL;
	mfc_get(list, eaJobList, 16, dmaTag, 0, 0);
	mfc_write_tag_mask(1u << dmaTag);
	(void)mfc_read_tag_status_all();
	n = list[0];
	size = list[1];
	if (size != 64 && ((size & 0x7f) || size > 1023))
		return (int)JOB_INVAL;
	if (sync && (rc = count_add(eaPort2, n)) != 0)
		return rc;
	get_line(eaPort2);
	rc = _cellSpursJobQueuePushJobListBody(P32(0x18), (int)P32(0x1c), eaJobList, tag, dmaTag,
	                                       sync ? eaPort2 : 0, !(flag & FLAG_NONBLOCK));
	if (rc && sync) {
		int rc2 = count_sub(eaPort2, n);
		if (rc2)
			return rc2;
	}
	return rc;
}

int cellSpursJobQueuePort2AllocateJobDescriptor(uint64_t eaPort2, size_t sizeDesc, unsigned int dmaTag, unsigned flag,
                                                uint64_t *eaAllocatedJobDesc)
{
	if (flag & ~FLAG_NONBLOCK)
		return (int)JOB_INVAL;
	if (!eaPort2 || !eaAllocatedJobDesc)
		return (int)JOB_NULL;
	if (eaPort2 & 0x7f)
		return (int)JOB_ALIGN;
	if (dmaTag > 31)
		return (int)JOB_INVAL;
	get_line(eaPort2);
	return _cellSpursJobQueueAllocateJobDescriptor(P32(0x18), (int)P32(0x1c), sizeDesc, dmaTag, flag,
	                                              eaAllocatedJobDesc);
}

/* take a pool descriptor (waiting for one if need be), copy the LS job into
   it and push it; the descriptor goes back to the pool when the job ends */
int _cellSpursJobQueuePort2CopyPushJobBody(uint64_t eaPort2, const void *pJob, size_t sizeDesc,
                                           size_t sizeDescFromPool, unsigned tag, unsigned int dmaTag,
                                           unsigned flag)
{
	const int sync = flag & FLAG_SYNC;
	uint64_t ea;
	int rc;
	if (flag & ~3u)
		return (int)JOB_INVAL;
	if (!eaPort2 || !pJob)
		return (int)JOB_NULL;
	if ((eaPort2 & 0x7f) || ((uintptr_t)pJob & 15))
		return (int)JOB_ALIGN;
	if ((sizeDesc != 64 && (sizeDesc & 0x7f)) || tag > 15 || sizeDesc > 1023 || dmaTag > 31)
		return (int)JOB_INVAL;
	if (sync && (rc = count_add(eaPort2, 1)) != 0)
		return rc;
	get_line(eaPort2);
	rc = _cellSpursJobQueueAllocateJobDescriptor(P32(0x18), (int)P32(0x1c), sizeDescFromPool, dmaTag, 0, &ea);
	if (!rc) {
		mfc_put((volatile void *)pJob, ea, sizeDesc, 0, 0, 0);
		mfc_write_tag_mask(1u << 0);
		(void)mfc_read_tag_status_all();
		get_line(eaPort2);
		rc = _cellSpursJobQueuePushAndReleaseJobBody(P32(0x18), (int)P32(0x1c), ea, sizeDesc, tag, dmaTag, flag,
		                                             sync ? eaPort2 : 0);
	}
	if (rc && sync) {
		int rc2 = count_sub(eaPort2, 1);
		if (rc2)
			return rc2;
	}
	return rc;
}

int cellSpursJobQueuePort2PushFlush(uint64_t eaPort2, unsigned int dmaTag, unsigned flag)
{
	int rc;
	if (flag & ~FLAG_NONBLOCK)
		return (int)JOB_INVAL;
	if ((rc = check_port(eaPort2)) != 0)
		return rc;
	if (dmaTag > 31)
		return (int)JOB_INVAL;
	get_line(eaPort2);
	return _cellSpursJobQueuePushFlush(P32(0x18), (int)P32(0x1c), dmaTag, !(flag & FLAG_NONBLOCK));
}

int cellSpursJobQueuePort2PushSync(uint64_t eaPort2, unsigned tagMask, unsigned int dmaTag, unsigned flag)
{
	int rc;
	if (flag & ~FLAG_NONBLOCK)
		return (int)JOB_INVAL;
	if ((rc = check_port(eaPort2)) != 0)
		return rc;
	if (dmaTag > 31)
		return (int)JOB_INVAL;
	get_line(eaPort2);
	return _cellSpursJobQueuePushSync(P32(0x18), (int)P32(0x1c), tagMask, dmaTag, !(flag & FLAG_NONBLOCK));
}

/* wait for every job pushed with sync since the last Port2Sync */
int cellSpursJobQueuePort2Sync(uint64_t eaPort2, unsigned flag)
{
	const int blocking = !(flag & FLAG_NONBLOCK);
	uint32_t count;
	int rc;
	if (flag & ~FLAG_NONBLOCK)
		return (int)JOB_INVAL;
	if ((rc = check_port(eaPort2)) != 0)
		return rc;
	if (blocking && _cellSpursTaskCanCallBlockWait())
		return (int)JOB_PERM;
	do {
		get_line(eaPort2);
		if (P8(0x14))
			return (int)JOB_BUSY;
		P8(0x14) = 1;
		count = P32(0x10);
	} while (!put_line(eaPort2));
	rc = blocking ? cellSpursJobQueueSemaphoreAcquire((uint32_t)eaPort2, count)
	              : cellSpursJobQueueSemaphoreTryAcquire((uint32_t)eaPort2, count);
	do {
		get_line(eaPort2);
		if (!rc)
			P32(0x10) -= count;
		P8(0x14) = 0;
	} while (!put_line(eaPort2));
	return rc;
}
