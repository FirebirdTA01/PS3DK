/* cell/spurs/job_queue_semaphore.h - PPU + SPU surface for the
 * job-queue semaphore primitive (counted release / acquire).
 *
 * The semaphore is bound to a job queue at Initialize time; SPU jobs
 * implicitly release a count when they finish, PPU consumers Acquire
 * to block until N jobs have completed.  SPU-side variants take
 * 32-bit EA arguments (the semaphore object lives in main memory and
 * SPU code reaches it via a 32-bit EA cast from uint64_t pointers).
 */
#ifndef __PS3DK_CELL_SPURS_JOB_QUEUE_SEMAPHORE_H__
#define __PS3DK_CELL_SPURS_JOB_QUEUE_SEMAPHORE_H__

#include <stdint.h>
#include <cell/spurs/job_queue_types.h>

#ifdef __cplusplus
extern "C" {
#endif

#ifndef __SPU__
extern int cellSpursJobQueueSemaphoreInitialize(CellSpursJobQueueSemaphore *pSemaphore,
                                                CellSpursJobQueue *pJobQueue);
extern int cellSpursJobQueueSemaphoreAcquire(CellSpursJobQueueSemaphore *pSemaphore,
                                             unsigned int acquireCount);
extern int cellSpursJobQueueSemaphoreTryAcquire(CellSpursJobQueueSemaphore *pSemaphore,
                                                unsigned int acquireCount);
#else /* __SPU__ */
extern int cellSpursJobQueueSemaphoreInitialize(uint32_t eaSemaphore,
                                                uint32_t eaJobQueue);
extern int cellSpursJobQueueSemaphoreAcquire(uint32_t eaSemaphore,
                                             unsigned int acquireCount);
extern int cellSpursJobQueueSemaphoreTryAcquire(uint32_t eaSemaphore,
                                                unsigned int acquireCount);
#endif

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* __PS3DK_CELL_SPURS_JOB_QUEUE_SEMAPHORE_H__ */
