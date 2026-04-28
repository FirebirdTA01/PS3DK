/* cell/spurs/job_queue_port2.h - SPURS job-queue Port2 PPU surface.
 *
 * Port2 is the multi-tag-aware revision of CellSpursJobQueuePort.
 * Differences vs the original port:
 *   - Create / Destroy lifecycle (no Initialize/Finalize naming)
 *   - Per-call `flag` parameter (CELL_SPURS_JOBQUEUE_FLAG_*) replaces
 *     the booleans (isSync, isExclusive, isBlocking, isMTSafe)
 *   - PushSync takes a tagMask argument
 *   - Adds AllocateJobDescriptor / PushAndReleaseJob for the
 *     allocate-fill-push pipeline that pairs with the descriptor pool
 *   - Adds CopyPushJob with a separate sizeDescFromPool that lets the
 *     caller request a larger pool slot than the source descriptor
 *
 * Each push entry validates port alignment + nullness on the inline
 * wrapper so callers get a fast NULL_POINTER / ALIGN return without a
 * cross-PRX call.
 */
#ifndef __PS3DK_CELL_SPURS_JOB_QUEUE_PORT2_H__
#define __PS3DK_CELL_SPURS_JOB_QUEUE_PORT2_H__

#include <stdint.h>
#include <stddef.h>

#include <cell/spurs/error.h>
#include <cell/spurs/job_descriptor.h>
#include <cell/spurs/job_queue.h>
#include <cell/spurs/job_queue_port2_types.h>

#ifndef __SPU__
#include <stdbool.h>
#endif

#ifdef __cplusplus
extern "C" {
#endif

#ifndef __SPU__
/* =====================================================================
 * PPU surface
 * =====================================================================*/

/* -- Information ----------------------------------------------------- */

extern CellSpursJobQueue *cellSpursJobQueuePort2GetJobQueue(
    const CellSpursJobQueuePort2 *pPort2);

/* -- Lifecycle ------------------------------------------------------- */

extern int cellSpursJobQueuePort2Create(CellSpursJobQueuePort2 *eaPort2,
                                        CellSpursJobQueue *eaJobQueue);
extern int cellSpursJobQueuePort2Destroy(CellSpursJobQueuePort2 *eaPort2);

/* -- Push body NIDs (used by the inline wrappers below) ------------- */

extern int _cellSpursJobQueuePort2PushJobBody(CellSpursJobQueuePort2 *eaPort2,
                                              CellSpursJobHeader *eaJob,
                                              size_t sizeDesc, unsigned tag,
                                              unsigned flag);

extern int _cellSpursJobQueuePort2CopyPushJobBody(CellSpursJobQueuePort2 *eaPort2,
                                                  const CellSpursJobHeader *pJob,
                                                  size_t sizeDesc,
                                                  size_t sizeDescFromPool,
                                                  unsigned tag, unsigned flag);

extern int _cellSpursJobQueuePort2PushJobListBody(CellSpursJobQueuePort2 *eaPort2,
                                                  CellSpursJobList *eaJobList,
                                                  unsigned tag, unsigned flag);

extern int _cellSpursJobQueuePort2PushAndReleaseJobBody(
    CellSpursJobQueuePort2 *eaPort2,
    CellSpursJobHeader *eaJob, size_t sizeDesc,
    unsigned tag, unsigned flag);

/* -- External push-side inline wrappers ------------------------------ */

#ifdef CELL_SPURS_JOBDESCRIPTOR_ERROR_CHECK
#  define _PS3DK_JQ_PORT2_CHECK(eaPort2, eaJob, sizeDesc)                    \
    __CELL_SPURS_RETURN_IF(cellSpursJobQueueCheckJob(                        \
        (const CellSpursJob256 *)(uintptr_t)(eaJob), (sizeDesc),             \
        cellSpursJobQueueGetMaxSizeJobDescriptor(                            \
            cellSpursJobQueuePort2GetJobQueue(eaPort2))))
#else
#  define _PS3DK_JQ_PORT2_CHECK(eaPort2, eaJob, sizeDesc) ((void)0)
#endif

#define _PS3DK_JQ_PORT2_VALIDATE(eaPort2)                                    \
    do {                                                                     \
        if ((eaPort2) == 0) return CELL_SPURS_JOB_ERROR_NULL_POINTER;        \
        if ((uintptr_t)(eaPort2) % CELL_SPURS_JOBQUEUE_PORT2_ALIGN)          \
            return CELL_SPURS_JOB_ERROR_ALIGN;                               \
    } while (0)

static inline int cellSpursJobQueuePort2PushJob(CellSpursJobQueuePort2 *eaPort2,
                                                CellSpursJobHeader *eaJob,
                                                size_t sizeDesc, unsigned tag,
                                                unsigned flag)
{
    _PS3DK_JQ_PORT2_CHECK(eaPort2, eaJob, sizeDesc);
    _PS3DK_JQ_PORT2_VALIDATE(eaPort2);
    return _cellSpursJobQueuePort2PushJobBody(eaPort2, eaJob, sizeDesc, tag, flag);
}

static inline int cellSpursJobQueuePort2CopyPushJob(CellSpursJobQueuePort2 *eaPort2,
                                                    const CellSpursJobHeader *eaJob,
                                                    size_t sizeDesc,
                                                    size_t sizeDescFromPool,
                                                    unsigned tag, unsigned flag)
{
    _PS3DK_JQ_PORT2_CHECK(eaPort2, eaJob, sizeDesc);
    _PS3DK_JQ_PORT2_VALIDATE(eaPort2);
    return _cellSpursJobQueuePort2CopyPushJobBody(eaPort2, eaJob, sizeDesc,
                                                  sizeDescFromPool, tag, flag);
}

static inline int cellSpursJobQueuePort2PushJobList(CellSpursJobQueuePort2 *eaPort2,
                                                    CellSpursJobList *eaJobList,
                                                    unsigned tag, unsigned flag)
{
#ifdef CELL_SPURS_JOBDESCRIPTOR_ERROR_CHECK
    for (unsigned int i = 0; i < eaJobList->numJobs; ++i) {
        CellSpursJob256 *eaJob = (CellSpursJob256 *)((uintptr_t)eaJobList->eaJobList
                                                      + eaJobList->sizeOfJob * i);
        __CELL_SPURS_RETURN_IF(cellSpursJobQueueCheckJob(
            (const CellSpursJob256 *)eaJob, eaJobList->sizeOfJob,
            cellSpursJobQueueGetMaxSizeJobDescriptor(
                cellSpursJobQueuePort2GetJobQueue(eaPort2))));
    }
#endif
    _PS3DK_JQ_PORT2_VALIDATE(eaPort2);
    return _cellSpursJobQueuePort2PushJobListBody(eaPort2, eaJobList, tag, flag);
}

static inline int cellSpursJobQueuePort2PushAndReleaseJob(CellSpursJobQueuePort2 *eaPort2,
                                                          CellSpursJobHeader *eaJob,
                                                          size_t sizeDesc,
                                                          unsigned tag, unsigned flag)
{
    _PS3DK_JQ_PORT2_CHECK(eaPort2, eaJob, sizeDesc);
    _PS3DK_JQ_PORT2_VALIDATE(eaPort2);
    return _cellSpursJobQueuePort2PushAndReleaseJobBody(eaPort2, eaJob, sizeDesc,
                                                        tag, flag);
}

/* -- Direct entry points (not inline-wrapped) ----------------------- */

extern int cellSpursJobQueuePort2AllocateJobDescriptor(
    CellSpursJobQueuePort2 *eaPort2, size_t sizeDesc, unsigned flag,
    CellSpursJobHeader **eaAllocatedJobDesc);

extern int cellSpursJobQueuePort2Sync(CellSpursJobQueuePort2 *eaPort2,
                                      unsigned flag);

extern int cellSpursJobQueuePort2PushFlush(CellSpursJobQueuePort2 *eaPort2,
                                           unsigned flag);

extern int cellSpursJobQueuePort2PushSync(CellSpursJobQueuePort2 *eaPort2,
                                          unsigned tagMask, unsigned flag);

#else /* __SPU__ */
/* =====================================================================
 * SPU surface
 *
 * Same operations as PPU but with 64-bit EA arguments and a per-call
 * dmaTag.  The descriptor-error-check inlines (cell/dma.h dependent)
 * are documented in the libspurs_jq SPU runtime; this header carries
 * the entry-point declarations only.
 * =====================================================================*/

extern uint64_t cellSpursJobQueuePort2GetJobQueue(uint64_t eaPort2);

extern int cellSpursJobQueuePort2Create(uint64_t eaPort2, uint64_t eaJobQueue);
extern int cellSpursJobQueuePort2Destroy(uint64_t eaPort2);

extern int _cellSpursJobQueuePort2PushJobBody(uint64_t eaPort2,
                                              uint64_t eaJob,
                                              size_t sizeDesc, unsigned tag,
                                              unsigned int dmaTag,
                                              unsigned flag,
                                              unsigned isAutoRelease);

extern int _cellSpursJobQueuePort2PushJobListBody(uint64_t eaPort2,
                                                  uint64_t eaJobList,
                                                  unsigned tag,
                                                  unsigned int dmaTag,
                                                  unsigned flag);

extern int _cellSpursJobQueuePort2CopyPushJobBody(uint64_t eaPort2,
                                                  const CellSpursJobHeader *pJob,
                                                  size_t sizeDesc,
                                                  size_t sizeDescFromPool,
                                                  unsigned tag,
                                                  unsigned int dmaTag,
                                                  unsigned flag);

extern int cellSpursJobQueuePort2AllocateJobDescriptor(
    uint64_t eaPort2, size_t sizeDesc,
    unsigned int dmaTag, unsigned flag,
    uint64_t *eaAllocatedJobDesc);

extern int cellSpursJobQueuePort2Sync(uint64_t eaPort2, unsigned flag);
extern int cellSpursJobQueuePort2PushFlush(uint64_t eaPort2, unsigned flag);
extern int cellSpursJobQueuePort2PushSync(uint64_t eaPort2,
                                          unsigned tagMask, unsigned flag);

#endif /* __SPU__ */

#ifdef __cplusplus
} /* extern "C" */
#endif

#include <cell/spurs/job_queue_port2_cpp_types.h>

#endif /* __PS3DK_CELL_SPURS_JOB_QUEUE_PORT2_H__ */
