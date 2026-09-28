/* SPU job-queue runtime: the job-queue semaphore, acquired by SPU tasks.
 *
 * CellSpursJobQueueSemaphore (128 bytes, main memory): u32 at +0x00 =
 * bit 31 a task waits, bit 29 set with it, bits 0..28 the signed count;
 * +0x04 the waiting task (taskset EA | task id); +0x08 the job queue EA.
 * An acquire that finds too little leaves the count negative, records
 * itself as the waiter and sleeps until a release makes the count good.
 * Independently written from the published object layouts and semantics.
 */
#include <stdint.h>
#include <spu_mfcio.h>

#define JOB_AGAIN  0x80410A01u
#define JOB_INVAL  0x80410A02u
#define JOB_PERM   0x80410A09u
#define JOB_BUSY   0x80410A0Au
#define JOB_ALIGN  0x80410A10u
#define JOB_NULL   0x80410A11u
#define TASK_FATAL 0x80410914u

extern uint64_t cellSpursGetTasksetAddress(void);
extern unsigned int cellSpursGetTaskId(void);
extern int cellSpursWaitSignal(void);
extern int _cellSpursTaskCanCallBlockWait(void);

static uint8_t line[128] __attribute__((aligned(128)));

static inline void line_get(uint64_t ea)
{
	mfc_getllar(line, ea, 0, 0);
	(void)mfc_read_atomic_status();
	spu_dsync();
}

static inline int line_put(uint64_t ea)
{
	spu_dsync();
	mfc_putllc(line, ea, 0, 0);
	return (mfc_read_atomic_status() & MFC_PUTLLC_STATUS) == 0;
}

#define U32(o) (*(volatile uint32_t *)(line + (o)))


#define SEM_WAITER   0x80000000u
#define SEM_WAITING  0x20000000u
#define SEM_COUNT    0x1fffffffu
#define MAX_ACQUIRE  0x0fffffffu

int cellSpursJobQueueSemaphoreInitialize(uint32_t eaSemaphore, uint32_t eaJobQueue)
{
	if (!eaSemaphore || !eaJobQueue)
		return (int)JOB_NULL;
	if ((eaSemaphore & 0x7f) || (eaJobQueue & 0x7f))
		return (int)JOB_ALIGN;
	do {
		line_get(eaSemaphore);
		U32(0x00) = 0;
		U32(0x04) = 0;
		U32(0x08) = eaJobQueue;
		U32(0x0c) = 0;
	} while (!line_put(eaSemaphore));
	return 0;
}

static int semaphore_acquire(uint32_t eaSemaphore, unsigned int count, int isBlocking)
{
	uint32_t waiter;
	int left, wait;
	if (!eaSemaphore)
		return (int)JOB_NULL;
	if (eaSemaphore & 0x7f)
		return (int)JOB_ALIGN;
	if (count > MAX_ACQUIRE)
		return (int)JOB_INVAL;
	if (isBlocking && _cellSpursTaskCanCallBlockWait())
		return (int)JOB_PERM;
	waiter = (uint32_t)cellSpursGetTasksetAddress() | cellSpursGetTaskId();
	do {
		uint32_t w;
		line_get(eaSemaphore);
		w = U32(0x00);
		if (w & SEM_WAITER)
			return (int)JOB_BUSY;
		left = ((int32_t)(w << 3) >> 3) - (int)count;
		wait = left < 0;
		if (wait) {
			if (!isBlocking)
				return (int)JOB_AGAIN;
			w = (w | SEM_WAITER | SEM_WAITING) & ~0x40000000u;
			U32(0x04) = waiter;
		}
		U32(0x00) = (w & ~SEM_COUNT) | ((uint32_t)left & SEM_COUNT);
	} while (!line_put(eaSemaphore));
	if (wait && cellSpursWaitSignal())
		return (int)TASK_FATAL;
	return 0;
}

int cellSpursJobQueueSemaphoreAcquire(uint32_t eaSemaphore, unsigned int acquireCount)
{
	return semaphore_acquire(eaSemaphore, acquireCount, 1);
}

int cellSpursJobQueueSemaphoreTryAcquire(uint32_t eaSemaphore, unsigned int acquireCount)
{
	return semaphore_acquire(eaSemaphore, acquireCount, 0);
}
