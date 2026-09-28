/* SPU job-queue runtime, part 3: job-queue ports (CellSpursJobQueuePort).
 *
 * A port is a 128-byte line in main memory that keeps a job-queue handle
 * and counts the jobs pushed through it "with sync", so PortSync can wait
 * for all of them.  Its first 16 bytes are a job-queue semaphore: a job
 * pushed with sync carries the port as its semaphore and releases one
 * count when it finishes.
 *   0x00      job-queue semaphore
 *   0x10 u32  jobs pushed with sync since the last PortSync
 *   0x14 u16  next free slot of the descriptor buffer
 *   0x16 u8   a PortSync is running
 *   0x18 u32  descriptor buffer EA (copy-push), 0x1c u16 its entries,
 *   0x1e u16  bytes per entry
 *   0x20 u32  job queue EA, 0x24 u32 job-queue handle
 *   0x28 u8   initialized, 0x29 u8 initializing / finalizing,
 *   0x2a u8   multi-thread safe
 * Independently written from the published object layouts and semantics.
 */
#include <stdint.h>
#include <spu_mfcio.h>

#define JOB_AGAIN  0x80410A01u
#define JOB_INVAL  0x80410A02u
#define JOB_PERM   0x80410A09u
#define JOB_BUSY   0x80410A0Au
#define JOB_STAT   0x80410A0Fu
#define JOB_ALIGN  0x80410A10u
#define JOB_NULL   0x80410A11u

#define MAX_COUNT  0x0ffffffeu

extern int cellSpursJobQueueOpen(uint64_t eaJobQueue, int *handle);
extern int cellSpursJobQueueClose(uint64_t eaJobQueue, int handle);
extern int cellSpursJobQueueSemaphoreInitialize(uint32_t eaSemaphore, uint32_t eaJobQueue);
extern int cellSpursJobQueueSemaphoreAcquire(uint32_t eaSemaphore, unsigned int count);
extern int cellSpursJobQueueSemaphoreTryAcquire(uint32_t eaSemaphore, unsigned int count);
extern int _cellSpursTaskCanCallBlockWait(void);
extern int _cellSpursJobQueuePushJobBody(uint64_t eaJobQueue, int handle, uint64_t eaJob, unsigned int sizeDesc,
                                         unsigned int tag, unsigned int dmaTag, uint64_t eaSemaphore,
                                         unsigned int isExclusive, unsigned int isBlocking);
extern int _cellSpursJobQueuePushJobListBody(uint64_t eaJobQueue, int handle, uint64_t eaJobList, unsigned int tag,
                                             unsigned int dmaTag, uint64_t eaSemaphore, unsigned int isBlocking);
extern int _cellSpursJobQueuePushFlush(uint64_t eaJobQueue, int handle, unsigned int dmaTag, unsigned int isBlocking);
extern int _cellSpursJobQueuePushSync(uint64_t eaJobQueue, int handle, unsigned int tagMask, unsigned int dmaTag,
                                      unsigned int isBlocking);

static uint8_t line[128] __attribute__((aligned(128)));

#define P8(o)  (*(volatile uint8_t *)(line + (o)))
#define P16(o) (*(volatile uint16_t *)(line + (o)))
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

static int check_desc(unsigned size)
{
	return size == 64 || (!(size & 0x7f) && size <= 1023);
}

uint64_t cellSpursJobQueuePortGetJobQueue(uint64_t eaPort)
{
	if (check_port(eaPort))
		return 0;
	get_line(eaPort);
	return P32(0x20);
}

int cellSpursJobQueuePortInitialize(uint64_t eaPort, uint64_t eaJobQueue, unsigned int isMTSafe)
{
	int handle = -1, rc;
	if (!eaPort || !eaJobQueue)
		return (int)JOB_NULL;
	if ((eaPort & 0x7f) || (eaJobQueue & 0x7f))
		return (int)JOB_ALIGN;
	do {
		get_line(eaPort);
		P8(0x29) = 1;
		P8(0x2a) = (uint8_t)isMTSafe;
	} while (!put_line(eaPort));
	if ((rc = cellSpursJobQueueOpen(eaJobQueue, &handle)) != 0)
		return rc;
	if ((rc = cellSpursJobQueueSemaphoreInitialize((uint32_t)eaPort, (uint32_t)eaJobQueue)) != 0) {
		(void)cellSpursJobQueueClose(eaJobQueue, handle);
		return rc;
	}
	do {
		get_line(eaPort);
		P32(0x20) = (uint32_t)eaJobQueue;
		P32(0x24) = (uint32_t)handle;
		P8(0x28) = 1;
		P8(0x29) = 0;
	} while (!put_line(eaPort));
	return 0;
}

int cellSpursJobQueuePortInitializeWithDescriptorBuffer(uint64_t eaPort, uint64_t eaJobQueue, uint64_t eaBuffer,
                                                        unsigned int sizeDesc, unsigned int numEntries,
                                                        unsigned int isMTSafe)
{
	int rc;
	if (!eaBuffer)
		return (int)JOB_NULL;
	if (eaBuffer & 0x7f)
		return (int)JOB_ALIGN;
	if (!check_desc(sizeDesc) || !numEntries)
		return (int)JOB_INVAL;
	if ((rc = cellSpursJobQueuePortInitialize(eaPort, eaJobQueue, isMTSafe)) != 0)
		return rc;
	do {
		get_line(eaPort);
		P32(0x18) = (uint32_t)eaBuffer;
		P16(0x1c) = (uint16_t)numEntries;
		P16(0x1e) = (uint16_t)sizeDesc;
	} while (!put_line(eaPort));
	return 0;
}

int cellSpursJobQueuePortFinalize(uint64_t eaPort)
{
	int rc = check_port(eaPort);
	uint32_t jq;
	int handle;
	if (rc)
		return rc;
	do {
		get_line(eaPort);
		jq = P32(0x20);
		handle = (int)P32(0x24);
		if (!P8(0x28) || P8(0x29) || !jq)
			return (int)JOB_STAT;
		P8(0x29) = 1;
	} while (!put_line(eaPort));
	if (cellSpursJobQueueClose(jq, handle))
		spu_stop(0);                        /* the port's handle must close */
	do {
		get_line(eaPort);
		P8(0x28) = 0;
		P8(0x29) = 0;
		P32(0x20) = 0;
	} while (!put_line(eaPort));
	return 0;
}

/* count one more job pushed with sync */
static int count_up(uint64_t port)
{
	do {
		get_line(port);
		if (P8(0x16) || P32(0x10) > MAX_COUNT)
			return (int)JOB_BUSY;
		P32(0x10) += 1;
	} while (!put_line(port));
	return 0;
}

static void count_down(uint64_t port)
{
	do {
		get_line(port);
		P32(0x10) -= 1;
	} while (!put_line(port));
}

static int port_push(uint64_t port, uint64_t eaJob, unsigned size, unsigned tag, unsigned dmaTag, unsigned isSync,
                     unsigned isExclusive, unsigned isBlocking)
{
	int rc;
	if (!port || !eaJob)
		return (int)JOB_NULL;
	if ((port & 0x7f) || (eaJob & 15))
		return (int)JOB_ALIGN;
	if (!check_desc(size) || tag > 15 || dmaTag > 31)
		return (int)JOB_INVAL;
	if (isSync && (rc = count_up(port)) != 0)
		return rc;
	get_line(port);
	rc = _cellSpursJobQueuePushJobBody(P32(0x20), (int)P32(0x24), eaJob, size, tag, dmaTag,
	                                   isSync ? port : 0, isExclusive, isBlocking);
	if (rc && isSync)
		count_down(port);
	return rc;
}

int _cellSpursJobQueuePortPushJobBody(uint64_t eaPort, uint64_t eaJob, unsigned int sizeDesc, unsigned int tag,
                                      unsigned int dmaTag, unsigned int isSync, unsigned int isExclusive,
                                      unsigned int isBlocking)
{
	return port_push(eaPort, eaJob, sizeDesc, tag, dmaTag, isSync, isExclusive, isBlocking);
}

int _cellSpursJobQueuePortPushBody(uint64_t eaPort, uint64_t eaJob, unsigned int sizeDesc, unsigned int dmaTag,
                                   unsigned int isSync, unsigned int isBlocking)
{
	return port_push(eaPort, eaJob, sizeDesc, 0, dmaTag, isSync, 0, isBlocking);
}

int _cellSpursJobQueuePortPushJobListBody(uint64_t eaPort, uint64_t eaJobList, unsigned int tag, unsigned int dmaTag,
                                          unsigned int isSync, unsigned int isBlocking)
{
	int rc;
	if (!eaPort || !eaJobList)
		return (int)JOB_NULL;
	if ((eaPort & 0x7f) || (eaJobList & 15))
		return (int)JOB_ALIGN;
	if (tag > 15 || dmaTag > 31)
		return (int)JOB_INVAL;
	if (isSync && (rc = count_up(eaPort)) != 0)
		return rc;
	get_line(eaPort);
	rc = _cellSpursJobQueuePushJobListBody(P32(0x20), (int)P32(0x24), eaJobList, tag, dmaTag,
	                                       isSync ? eaPort : 0, isBlocking);
	if (rc && isSync)
		count_down(eaPort);
	return rc;
}

/* copy an LS descriptor into the port's descriptor buffer and push it */
static int port_copy_push(uint64_t port, const void *pJob, unsigned size, unsigned tag, unsigned dmaTag,
                          unsigned isSync, unsigned isExclusive, unsigned isBlocking)
{
	uint64_t slotEa;
	int rc;
	if (!port || !pJob)
		return (int)JOB_NULL;
	if ((port & 0x7f) || ((uintptr_t)pJob & 15))
		return (int)JOB_ALIGN;
	if (!check_desc(size) || tag > 15 || dmaTag > 31)
		return (int)JOB_INVAL;
	do {
		unsigned slot;
		get_line(port);
		if (!P32(0x18))
			return (int)JOB_PERM;           /* no descriptor buffer */
		if (size > P16(0x1e))
			return (int)JOB_INVAL;
		if (P8(0x16) || P32(0x10) > MAX_COUNT)
			return (int)JOB_BUSY;
		slot = P16(0x14);
		if (slot == P16(0x1c))
			return (int)JOB_AGAIN;          /* buffer used up until a PortSync */
		slotEa = P32(0x18) + (uint64_t)slot * P16(0x1e);
		P16(0x14) = (uint16_t)(slot + 1);
		if (isSync)
			P32(0x10) += 1;
	} while (!put_line(port));
	mfc_put((volatile void *)pJob, slotEa, size, dmaTag, 0, 0);
	mfc_write_tag_mask(1u << dmaTag);
	(void)mfc_read_tag_status_all();
	get_line(port);
	rc = _cellSpursJobQueuePushJobBody(P32(0x20), (int)P32(0x24), slotEa, size, tag, dmaTag,
	                                   isSync ? port : 0, isExclusive, isBlocking);
	if (rc && isSync)
		count_down(port);
	return rc;
}

int _cellSpursJobQueuePortCopyPushJobBody(uint64_t eaPort, const void *pJob, unsigned int sizeDesc, unsigned int tag,
                                          unsigned int dmaTag, unsigned int isSync, unsigned int isExclusive,
                                          unsigned int isBlocking)
{
	return port_copy_push(eaPort, pJob, sizeDesc, tag, dmaTag, isSync, isExclusive, isBlocking);
}

int _cellSpursJobQueuePortCopyPushBody(uint64_t eaPort, const void *pJob, unsigned int sizeDesc, unsigned int dmaTag,
                                       unsigned int isSync, unsigned int isBlocking)
{
	return port_copy_push(eaPort, pJob, sizeDesc, 0, dmaTag, isSync, 0, isBlocking);
}

int _cellSpursJobQueuePortPushFlush(uint64_t eaPort, unsigned int dmaTag, unsigned int isBlocking)
{
	int rc = check_port(eaPort);
	if (rc)
		return rc;
	if (dmaTag > 31)
		return (int)JOB_INVAL;
	get_line(eaPort);
	return _cellSpursJobQueuePushFlush(P32(0x20), (int)P32(0x24), dmaTag, isBlocking);
}

int _cellSpursJobQueuePortPushSync(uint64_t eaPort, unsigned int tagMask, unsigned int dmaTag, unsigned int isBlocking)
{
	int rc = check_port(eaPort);
	if (rc)
		return rc;
	if (dmaTag > 31)
		return (int)JOB_INVAL;
	get_line(eaPort);
	return _cellSpursJobQueuePushSync(P32(0x20), (int)P32(0x24), tagMask, dmaTag, isBlocking);
}

/* wait for every job pushed with sync since the last PortSync */
static int port_sync(uint64_t port, int isBlocking)
{
	uint32_t count;
	uint16_t slots;
	int rc = check_port(port);
	if (rc)
		return rc;
	if (isBlocking && _cellSpursTaskCanCallBlockWait())
		return (int)JOB_PERM;
	do {
		get_line(port);
		if (P8(0x16))
			return (int)JOB_BUSY;
		P8(0x16) = 1;
		count = P32(0x10);
		slots = P16(0x14);
	} while (!put_line(port));
	rc = isBlocking ? cellSpursJobQueueSemaphoreAcquire((uint32_t)port, count)
	                : cellSpursJobQueueSemaphoreTryAcquire((uint32_t)port, count);
	do {
		get_line(port);
		P8(0x16) = 0;
		if (!rc) {
			P32(0x10) = 0;                  /* all done: the buffer is free again */
			P16(0x14) = 0;
		} else {
			P32(0x10) = count;
			P16(0x14) = slots;
		}
	} while (!put_line(port));
	return rc;
}

int cellSpursJobQueuePortSync(uint64_t eaPort)    { return port_sync(eaPort, 1); }
int cellSpursJobQueuePortTrySync(uint64_t eaPort) { return port_sync(eaPort, 0); }
