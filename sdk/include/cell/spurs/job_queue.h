/* cell/spurs/job_queue.h - SPURS job-queue PPU + SPU entry-point surface.
 *
 * A job queue is a multi-producer, multi-consumer SPU-resident
 * workload built on top of SPURS.  Producers attach to the queue
 * through ports (see cell/spurs/job_queue_port.h /
 * job_queue_port2.h), allocate / push job descriptors, and optionally
 * synchronise on tag masks.
 *
 * On the PPU side this file exposes attribute setup, queue lifecycle
 * (Create/Shutdown/Join), legacy non-port push variants, info access,
 * waiting-mode + exception-handler control, and the no-port push fast
 * path that uses an already-opened CellSpursJobQueueHandle.
 *
 * On the SPU side the same header exposes the EA-based SPU-callable
 * variants of the same operations (uint64_t EA arguments + dmaTag),
 * the cooperative-yield DMA-wait inline helpers (gated on
 * cell/dma.h being available), and the trace / memcheck globals the
 * SPU runtime publishes.
 *
 * All PPU entry points resolve to NIDs in libspurs_jq_stub.a; the
 * cellSpursJq sysmodule (CELL_SYSMODULE_SPURS_JQ) MUST be loaded once
 * before any Create call.
 */
#ifndef __PS3DK_CELL_SPURS_JOB_QUEUE_H__
#define __PS3DK_CELL_SPURS_JOB_QUEUE_H__

#include <stdint.h>
#include <stddef.h>

#include <cell/spurs/types.h>
#include <cell/spurs/error.h>
#include <cell/spurs/version.h>
#include <cell/spurs/job_descriptor.h>
#include <cell/spurs/job_queue_define.h>
#include <cell/spurs/job_queue_types.h>

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

/* -- Attribute setters ----------------------------------------------- */

extern int cellSpursJobQueueAttributeInitialize(CellSpursJobQueueAttribute *attr);

extern int cellSpursJobQueueAttributeSetMaxGrab(CellSpursJobQueueAttribute *attr,
                                                unsigned int maxGrab);
extern int cellSpursJobQueueAttributeSetSubmitWithEntryLock(CellSpursJobQueueAttribute *attr,
                                                            bool submitWithEntryLock);
extern int cellSpursJobQueueAttributeSetDoBusyWaiting(CellSpursJobQueueAttribute *attr,
                                                      bool doBusyWaiting);
extern int cellSpursJobQueueAttributeSetIsHaltOnError(CellSpursJobQueueAttribute *attr,
                                                      bool isHaltOnError);
extern int cellSpursJobQueueAttributeSetIsJobTypeMemoryCheck(CellSpursJobQueueAttribute *attr,
                                                             bool isJobTypeMemoryCheck);
extern int cellSpursJobQueueAttributeSetMaxSizeJobDescriptor(CellSpursJobQueueAttribute *attr,
                                                             unsigned int maxSizeJobDescriptor);
extern int cellSpursJobQueueAttributeSetGrabParameters(CellSpursJobQueueAttribute *attr,
                                                       unsigned int maxNumJobsOnSpu,
                                                       unsigned int maxGrab);

/* -- Descriptor-pool size helper ------------------------------------- */

static inline int cellSpursJobQueueGetJobDescriptorPoolSize(
    CellSpursJobQueueJobDescriptorPool *pPoolDesc)
{
    if (pPoolDesc == 0) return CELL_SPURS_JOB_ERROR_NULL_POINTER;
    if (pPoolDesc->nJob64  < 0 || pPoolDesc->nJob128 < 0 ||
        pPoolDesc->nJob256 < 0 || pPoolDesc->nJob384 < 0 ||
        pPoolDesc->nJob512 < 0 || pPoolDesc->nJob640 < 0 ||
        pPoolDesc->nJob768 < 0 || pPoolDesc->nJob896 < 0)
        return CELL_SPURS_JOB_ERROR_INVAL;
    return CELL_SPURS_JOBQUEUE_JOB_DESCRIPTOR_POOL_SIZE(
        pPoolDesc->nJob64,  pPoolDesc->nJob128,
        pPoolDesc->nJob256, pPoolDesc->nJob384,
        pPoolDesc->nJob512, pPoolDesc->nJob640,
        pPoolDesc->nJob768, pPoolDesc->nJob896);
}

/* -- Lifecycle ------------------------------------------------------- */

extern int _cellSpursCreateJobQueue(unsigned int jqRevision,
                                    unsigned int sdkRevision,
                                    CellSpurs *pSpurs,
                                    CellSpursJobQueue *pJobQueue,
                                    const CellSpursJobQueueAttribute *attr,
                                    const char *pName,
                                    uint64_t *pCommandList,
                                    unsigned int depth,
                                    unsigned int numSpus,
                                    const uint8_t priorityTable[CELL_SPURS_MAX_SPU]);

extern int _cellSpursCreateJobQueueWithJobDescriptorPool(
    unsigned int jqRevision,
    unsigned int sdkRevision,
    CellSpurs *pSpurs,
    CellSpursJobQueue *pJobQueue,
    const CellSpursJobQueueAttribute *attr,
    void *pPool,
    const CellSpursJobQueueJobDescriptorPool *pPoolDesc,
    const char *pName,
    uint64_t *pCommandList,
    unsigned int depth,
    unsigned int numSpus,
    const uint8_t priorityTable[CELL_SPURS_MAX_SPU]);

static inline int cellSpursCreateJobQueue(CellSpurs *pSpurs,
                                          CellSpursJobQueue *pJobQueue,
                                          const CellSpursJobQueueAttribute *attr,
                                          const char *pName,
                                          uint64_t *pCommandList,
                                          unsigned int depth,
                                          unsigned int numSpus,
                                          const uint8_t priorityTable[CELL_SPURS_MAX_SPU])
{
    return _cellSpursCreateJobQueue(CELL_SPURS_JOBQUEUE_REVISION,
                                    _CELL_SPURS_JQ_INTERNAL_VERSION,
                                    pSpurs, pJobQueue, attr, pName,
                                    pCommandList, depth, numSpus, priorityTable);
}

static inline int cellSpursCreateJobQueueWithJobDescriptorPool(
    CellSpurs *pSpurs,
    CellSpursJobQueue *pJobQueue,
    const CellSpursJobQueueAttribute *attr,
    void *pPool,
    const CellSpursJobQueueJobDescriptorPool *pPoolDesc,
    const char *pName,
    uint64_t *pCommandList,
    unsigned int depth,
    unsigned int numSpus,
    const uint8_t priorityTable[CELL_SPURS_MAX_SPU])
{
    return _cellSpursCreateJobQueueWithJobDescriptorPool(
        CELL_SPURS_JOBQUEUE_REVISION,
        _CELL_SPURS_JQ_INTERNAL_VERSION,
        pSpurs, pJobQueue, attr, pPool, pPoolDesc, pName,
        pCommandList, depth, numSpus, priorityTable);
}

extern int cellSpursShutdownJobQueue(CellSpursJobQueue *pJobQueue);
extern int cellSpursJoinJobQueue(CellSpursJobQueue *pJobQueue, int *exitCode);

extern int cellSpursJobQueueOpen(CellSpursJobQueue *pJobQueue,
                                 CellSpursJobQueueHandle *handle);
extern int cellSpursJobQueueClose(CellSpursJobQueue *pJobQueue,
                                  CellSpursJobQueueHandle handle);

/* -- Information ----------------------------------------------------- */

extern CellSpurs *cellSpursJobQueueGetSpurs(const CellSpursJobQueue *pJobQueue);
extern int cellSpursJobQueueGetHandleCount(const CellSpursJobQueue *pJobQueue);
extern int cellSpursJobQueueGetError(const CellSpursJobQueue *pJobQueue,
                                     int *exitCode, void **cause);
extern int cellSpursJobQueueGetMaxSizeJobDescriptor(const CellSpursJobQueue *pJobQueue);
extern int cellSpursGetJobQueueId(const CellSpursJobQueue *pJobQueue,
                                  CellSpursWorkloadId *pId);
extern int cellSpursJobQueueGetSuspendedJobSize(
    const CellSpursJobHeader *pJob,
    size_t sizeJobDesc,
    enum CellSpursJobQueueSuspendedJobAttribute attr,
    unsigned int *pSize);

/* -- Exception handling --------------------------------------------- */

extern int cellSpursJobQueueSetExceptionEventHandler(
    CellSpursJobQueue *pJobQueue,
    CellSpursJobQueueExceptionEventHandler handler,
    void *arg);
extern int cellSpursJobQueueSetExceptionEventHandler2(
    CellSpursJobQueue *pJobQueue,
    CellSpursJobQueueExceptionEventHandler2 handler,
    void *arg);
extern int cellSpursJobQueueUnsetExceptionEventHandler(CellSpursJobQueue *pJobQueue);

/* -- State control -------------------------------------------------- */

extern int cellSpursJobQueueSetWaitingMode(CellSpursJobQueue *pJobQueue,
                                           unsigned mode);

/* -- Push body NIDs (used by the inline wrappers below) ------------- */

extern int _cellSpursJobQueueAllocateJobDescriptorBody(
    CellSpursJobQueue *eaJobQueue,
    CellSpursJobQueueHandle handle,
    size_t sizeJobDesc, unsigned flag,
    CellSpursJobHeader **eaAllocatedJobDesc);

extern int _cellSpursJobQueuePushAndReleaseJobBody(
    CellSpursJobQueue *eaJobQueue,
    CellSpursJobQueueHandle handle,
    CellSpursJobHeader *eaJob, size_t sizeJobDesc,
    unsigned tag, unsigned flag,
    CellSpursJobQueueSemaphore *eaSemaphore);

extern int _cellSpursJobQueuePushJob2Body(
    CellSpursJobQueue *eaJobQueue,
    CellSpursJobQueueHandle handle,
    CellSpursJobHeader *eaJob, size_t sizeJobDesc,
    unsigned tag, unsigned flag,
    CellSpursJobQueueSemaphore *eaSemaphore);

extern int _cellSpursJobQueuePushJobBody2(
    CellSpursJobQueue *pJobQueue,
    CellSpursJobQueueHandle handle,
    CellSpursJobHeader *pJob, size_t sizeJobDesc,
    unsigned tag, CellSpursJobQueueSemaphore *pSemaphore,
    bool isExclusive, bool isBlocking);

extern int _cellSpursJobQueuePushJobListBody(
    CellSpursJobQueue *pJobQueue,
    CellSpursJobQueueHandle handle,
    CellSpursJobList *pJoblist, unsigned tag,
    CellSpursJobQueueSemaphore *pSemaphore, bool isBlocking);

extern int _cellSpursJobQueuePushFlush(CellSpursJobQueue *pJobQueue,
                                       CellSpursJobQueueHandle handle,
                                       bool isBlocking);
extern int _cellSpursJobQueuePushSync(CellSpursJobQueue *pJobQueue,
                                      CellSpursJobQueueHandle handle,
                                      unsigned tagMask, bool isBlocking);

/* Backwards-compatibility variants (older PRX revisions). */
extern int _cellSpursJobQueuePushBody(CellSpursJobQueue *pJobQueue,
                                      CellSpursJobQueueHandle handle,
                                      CellSpursJobHeader *pJob,
                                      size_t sizeJobDesc,
                                      CellSpursJobQueueSemaphore *pSemaphore,
                                      bool isBlocking);
extern int _cellSpursJobQueuePushJobBody(CellSpursJobQueue *pJobQueue,
                                         CellSpursJobQueueHandle handle,
                                         CellSpursJobHeader *pJob,
                                         size_t sizeJobDesc,
                                         unsigned tag,
                                         CellSpursJobQueueSemaphore *pSemaphore,
                                         bool isBlocking);

/* -- External push-side inline wrappers ------------------------------ */

#ifdef CELL_SPURS_JOBDESCRIPTOR_ERROR_CHECK
#  define _PS3DK_JQ_CHECK(pJobQueue, pJob, sizeJobDesc)                      \
    __CELL_SPURS_RETURN_IF(cellSpursJobQueueCheckJob(                        \
        (const CellSpursJob256 *)(uintptr_t)(pJob), (sizeJobDesc),           \
        cellSpursJobQueueGetMaxSizeJobDescriptor(pJobQueue)))
#else
#  define _PS3DK_JQ_CHECK(pJobQueue, pJob, sizeJobDesc) ((void)0)
#endif

static inline int cellSpursJobQueueAllocateJobDescriptor(
    CellSpursJobQueue *eaJobQueue, CellSpursJobQueueHandle handle,
    size_t sizeJobDesc, unsigned flag, CellSpursJobHeader **eaAllocatedJobDesc)
{
    return _cellSpursJobQueueAllocateJobDescriptorBody(
        eaJobQueue, handle, sizeJobDesc, flag, eaAllocatedJobDesc);
}

static inline int cellSpursJobQueuePushAndReleaseJob(
    CellSpursJobQueue *eaJobQueue, CellSpursJobQueueHandle handle,
    CellSpursJobHeader *eaJob, size_t sizeJobDesc, unsigned tag,
    unsigned flag, CellSpursJobQueueSemaphore *eaSemaphore)
{
    _PS3DK_JQ_CHECK(eaJobQueue, eaJob, sizeJobDesc);
    return _cellSpursJobQueuePushAndReleaseJobBody(
        eaJobQueue, handle, eaJob, sizeJobDesc, tag, flag, eaSemaphore);
}

static inline int cellSpursJobQueuePushJob2(
    CellSpursJobQueue *eaJobQueue, CellSpursJobQueueHandle handle,
    CellSpursJobHeader *eaJob, size_t sizeJobDesc, unsigned tag,
    unsigned flag, CellSpursJobQueueSemaphore *eaSemaphore)
{
    _PS3DK_JQ_CHECK(eaJobQueue, eaJob, sizeJobDesc);
    return _cellSpursJobQueuePushJob2Body(
        eaJobQueue, handle, eaJob, sizeJobDesc, tag, flag, eaSemaphore);
}

static inline int cellSpursJobQueuePush(
    CellSpursJobQueue *pJobQueue, CellSpursJobQueueHandle handle,
    CellSpursJobHeader *pJob, size_t sizeJobDesc,
    CellSpursJobQueueSemaphore *pSemaphore)
{
    _PS3DK_JQ_CHECK(pJobQueue, pJob, sizeJobDesc);
    return _cellSpursJobQueuePushJobBody2(
        pJobQueue, handle, pJob, sizeJobDesc, 0, pSemaphore, false, true);
}

static inline int cellSpursJobQueuePushJob(
    CellSpursJobQueue *pJobQueue, CellSpursJobQueueHandle handle,
    CellSpursJobHeader *pJob, size_t sizeJobDesc, unsigned tag,
    CellSpursJobQueueSemaphore *pSemaphore)
{
    _PS3DK_JQ_CHECK(pJobQueue, pJob, sizeJobDesc);
    return _cellSpursJobQueuePushJobBody2(
        pJobQueue, handle, pJob, sizeJobDesc, tag, pSemaphore, false, true);
}

static inline int cellSpursJobQueuePushExclusiveJob(
    CellSpursJobQueue *pJobQueue, CellSpursJobQueueHandle handle,
    CellSpursJobHeader *pJob, size_t sizeJobDesc, unsigned tag,
    CellSpursJobQueueSemaphore *pSemaphore)
{
    _PS3DK_JQ_CHECK(pJobQueue, pJob, sizeJobDesc);
    return _cellSpursJobQueuePushJobBody2(
        pJobQueue, handle, pJob, sizeJobDesc, tag, pSemaphore, true, true);
}

static inline int cellSpursJobQueueTryPush(
    CellSpursJobQueue *pJobQueue, CellSpursJobQueueHandle handle,
    CellSpursJobHeader *pJob, size_t sizeJobDesc,
    CellSpursJobQueueSemaphore *pSemaphore)
{
    _PS3DK_JQ_CHECK(pJobQueue, pJob, sizeJobDesc);
    return _cellSpursJobQueuePushJobBody2(
        pJobQueue, handle, pJob, sizeJobDesc, 0, pSemaphore, false, false);
}

static inline int cellSpursJobQueueTryPushJob(
    CellSpursJobQueue *pJobQueue, CellSpursJobQueueHandle handle,
    CellSpursJobHeader *pJob, size_t sizeJobDesc, unsigned tag,
    CellSpursJobQueueSemaphore *pSemaphore)
{
    _PS3DK_JQ_CHECK(pJobQueue, pJob, sizeJobDesc);
    return _cellSpursJobQueuePushJobBody2(
        pJobQueue, handle, pJob, sizeJobDesc, tag, pSemaphore, false, false);
}

static inline int cellSpursJobQueueTryPushExclusiveJob(
    CellSpursJobQueue *pJobQueue, CellSpursJobQueueHandle handle,
    CellSpursJobHeader *pJob, size_t sizeJobDesc, unsigned tag,
    CellSpursJobQueueSemaphore *pSemaphore)
{
    _PS3DK_JQ_CHECK(pJobQueue, pJob, sizeJobDesc);
    return _cellSpursJobQueuePushJobBody2(
        pJobQueue, handle, pJob, sizeJobDesc, tag, pSemaphore, true, false);
}

static inline int cellSpursJobQueuePushJobList(
    CellSpursJobQueue *pJobQueue, CellSpursJobQueueHandle handle,
    CellSpursJobList *pJoblist, unsigned tag,
    CellSpursJobQueueSemaphore *pSemaphore)
{
#ifdef CELL_SPURS_JOBDESCRIPTOR_ERROR_CHECK
    for (unsigned int i = 0; i < pJoblist->numJobs; ++i) {
        CellSpursJob256 *pJob = (CellSpursJob256 *)((uintptr_t)pJoblist->eaJobList
                                                     + pJoblist->sizeOfJob * i);
        __CELL_SPURS_RETURN_IF(cellSpursJobQueueCheckJob(
            (const CellSpursJob256 *)pJob, pJoblist->sizeOfJob,
            cellSpursJobQueueGetMaxSizeJobDescriptor(pJobQueue)));
    }
#endif
    return _cellSpursJobQueuePushJobListBody(
        pJobQueue, handle, pJoblist, tag, pSemaphore, true);
}

static inline int cellSpursJobQueueTryPushJobList(
    CellSpursJobQueue *pJobQueue, CellSpursJobQueueHandle handle,
    CellSpursJobList *pJoblist, unsigned tag,
    CellSpursJobQueueSemaphore *pSemaphore)
{
#ifdef CELL_SPURS_JOBDESCRIPTOR_ERROR_CHECK
    for (unsigned int i = 0; i < pJoblist->numJobs; ++i) {
        CellSpursJob256 *pJob = (CellSpursJob256 *)((uintptr_t)pJoblist->eaJobList
                                                     + pJoblist->sizeOfJob * i);
        __CELL_SPURS_RETURN_IF(cellSpursJobQueueCheckJob(
            (const CellSpursJob256 *)pJob, pJoblist->sizeOfJob,
            cellSpursJobQueueGetMaxSizeJobDescriptor(pJobQueue)));
    }
#endif
    return _cellSpursJobQueuePushJobListBody(
        pJobQueue, handle, pJoblist, tag, pSemaphore, false);
}

static inline int cellSpursJobQueuePushFlush(CellSpursJobQueue *pJobQueue,
                                             CellSpursJobQueueHandle handle)
{
    return _cellSpursJobQueuePushFlush(pJobQueue, handle, true);
}

static inline int cellSpursJobQueueTryPushFlush(CellSpursJobQueue *pJobQueue,
                                                CellSpursJobQueueHandle handle)
{
    return _cellSpursJobQueuePushFlush(pJobQueue, handle, false);
}

static inline int cellSpursJobQueuePushSync(CellSpursJobQueue *pJobQueue,
                                            CellSpursJobQueueHandle handle,
                                            unsigned tagMask)
{
    return _cellSpursJobQueuePushSync(pJobQueue, handle, tagMask, true);
}

static inline int cellSpursJobQueueTryPushSync(CellSpursJobQueue *pJobQueue,
                                               CellSpursJobQueueHandle handle,
                                               unsigned tag)
{
    return _cellSpursJobQueuePushSync(pJobQueue, handle, tag, false);
}

extern int cellSpursJobQueueSendSignal(CellSpursJobQueueWaitingJob *job);

#else /* __SPU__ */
/* =====================================================================
 * SPU surface
 *
 * SPU side takes 64-bit EAs (uint64_t) for queue / job pointers.
 * Push variants additionally take a `dmaTag` argument used both for
 * any kick-off DMAs the runtime issues internally and for the
 * descriptor-error-check `cellDmaGet` of the candidate job (when
 * CELL_SPURS_JOBDESCRIPTOR_ERROR_CHECK is defined).
 *
 * The cooperative-yield DMA-wait helpers
 * (cellSpursJobQueueDmaWaitTagStatus{All,Any}) and the SPU-side
 * descriptor-error-check inlines are documented in the libspurs_jq
 * SPU runtime; this header carries only the entry-point declarations
 * and the trace / memcheck globals so application code can compile
 * even before that runtime is fully ported.
 * =====================================================================*/

#include <cell/spurs/job_context.h>

/* -- Information / lifecycle ---------------------------------------- */

extern uint64_t cellSpursJobQueueGetSpurs(uint64_t eaJobQueue);
extern int cellSpursJobQueueGetHandleCount(uint64_t eaJobQueue);
extern int cellSpursJobQueueGetError(uint64_t eaJobQueue, int *exitCode, void **cause);
extern int cellSpursJobQueueGetMaxSizeJobDescriptor(uint64_t eaJobQueue);
extern int cellSpursGetJobQueueId(uint64_t eaJobQueue, CellSpursWorkloadId *pId);
extern int cellSpursJobQueueGetSuspendedJobSize(
    const CellSpursJobHeader *pJob, size_t sizeJobDesc,
    enum CellSpursJobQueueSuspendedJobAttribute attr,
    unsigned int *pSize);

extern int cellSpursJobQueueOpen(uint64_t eaJobQueue, CellSpursJobQueueHandle *handle);
extern int cellSpursJobQueueClose(uint64_t eaJobQueue, CellSpursJobQueueHandle handle);

/* -- Push body NIDs ------------------------------------------------- */

extern int _cellSpursJobQueueAllocateJobDescriptor(
    uint64_t eaJobQueue, CellSpursJobQueueHandle handle,
    size_t sizeJobDesc, unsigned dmaTag, unsigned flag,
    uint64_t *eaAllocatedJobDesc);

extern int _cellSpursJobQueuePushAndReleaseJobBody(
    uint64_t eaJobQueue, CellSpursJobQueueHandle handle,
    uint64_t eaJobHeader, size_t sizeJobDesc,
    unsigned tag, unsigned dmaTag, unsigned flag,
    uint64_t eaSemaphore);

extern int _cellSpursJobQueuePushJob2Body(
    uint64_t eaJobQueue, CellSpursJobQueueHandle handle,
    uint64_t eaJobHeader, size_t sizeJobDesc,
    unsigned tag, unsigned dmaTag, unsigned flag,
    uint64_t eaSemaphore);

extern int _cellSpursJobQueuePushJobBody(
    uint64_t eaJobQueue, CellSpursJobQueueHandle handle,
    uint64_t eaJobHeader, size_t sizeJobDesc,
    unsigned tag, unsigned dmaTag, uint64_t eaSemaphore,
    unsigned isExclusive, unsigned isBlocking);

extern int _cellSpursJobQueuePushJobListBody(
    uint64_t eaJobQueue, CellSpursJobQueueHandle handle,
    uint64_t eaJobList, unsigned tag, unsigned dmaTag,
    uint64_t eaSemaphore, unsigned isBlocking);

extern int _cellSpursJobQueuePushFlush(uint64_t eaJobQueue,
                                       CellSpursJobQueueHandle handle,
                                       unsigned dmaTag, unsigned isBlocking);

extern int _cellSpursJobQueuePushSync(uint64_t eaJobQueue,
                                      CellSpursJobQueueHandle handle,
                                      unsigned tagMask, unsigned dmaTag,
                                      unsigned isBlocking);

/* -- SPU-side push-fast-path inline wrappers ------------------------ */

static inline int cellSpursJobQueueAllocateJobDescriptor(
    uint64_t eaJobQueue, CellSpursJobQueueHandle handle,
    size_t sizeJobDesc, unsigned dmaTag, unsigned flag,
    uint64_t *eaAllocatedJobDesc)
{
    return _cellSpursJobQueueAllocateJobDescriptor(
        eaJobQueue, handle, sizeJobDesc, dmaTag, flag, eaAllocatedJobDesc);
}

static inline int cellSpursJobQueuePushAndReleaseJob(
    uint64_t eaJobQueue, CellSpursJobQueueHandle handle,
    uint64_t eaJobHeader, size_t sizeJobDesc, unsigned tag,
    unsigned dmaTag, unsigned flag, uint64_t eaSemaphore)
{
    return _cellSpursJobQueuePushAndReleaseJobBody(
        eaJobQueue, handle, eaJobHeader, sizeJobDesc, tag, dmaTag, flag, eaSemaphore);
}

static inline int cellSpursJobQueuePushJob2(
    uint64_t eaJobQueue, CellSpursJobQueueHandle handle,
    uint64_t eaJobHeader, size_t sizeJobDesc, unsigned tag,
    unsigned dmaTag, unsigned flag, uint64_t eaSemaphore)
{
    return _cellSpursJobQueuePushJob2Body(
        eaJobQueue, handle, eaJobHeader, sizeJobDesc, tag, dmaTag, flag, eaSemaphore);
}

static inline int cellSpursJobQueuePush(uint64_t eaJobQueue,
                                        CellSpursJobQueueHandle handle,
                                        uint64_t eaJobHeader, size_t sizeJobDesc,
                                        unsigned dmaTag, uint64_t eaSemaphore)
{
    return _cellSpursJobQueuePushJobBody(
        eaJobQueue, handle, eaJobHeader, sizeJobDesc, 0, dmaTag, eaSemaphore, 0, 1);
}

static inline int cellSpursJobQueuePushJob(uint64_t eaJobQueue,
                                           CellSpursJobQueueHandle handle,
                                           uint64_t eaJobHeader,
                                           size_t sizeJobDesc, unsigned tag,
                                           unsigned dmaTag, uint64_t eaSemaphore)
{
    return _cellSpursJobQueuePushJobBody(
        eaJobQueue, handle, eaJobHeader, sizeJobDesc, tag, dmaTag, eaSemaphore, 0, 1);
}

static inline int cellSpursJobQueueTryPushJob(uint64_t eaJobQueue,
                                              CellSpursJobQueueHandle handle,
                                              uint64_t eaJobHeader,
                                              size_t sizeJobDesc, unsigned tag,
                                              unsigned dmaTag, uint64_t eaSemaphore)
{
    return _cellSpursJobQueuePushJobBody(
        eaJobQueue, handle, eaJobHeader, sizeJobDesc, tag, dmaTag, eaSemaphore, 0, 0);
}

static inline int cellSpursJobQueuePushExclusiveJob(uint64_t eaJobQueue,
                                                    CellSpursJobQueueHandle handle,
                                                    uint64_t eaJobHeader,
                                                    size_t sizeJobDesc, unsigned tag,
                                                    unsigned dmaTag, uint64_t eaSemaphore)
{
    return _cellSpursJobQueuePushJobBody(
        eaJobQueue, handle, eaJobHeader, sizeJobDesc, tag, dmaTag, eaSemaphore, 1, 1);
}

static inline int cellSpursJobQueueTryPushExclusiveJob(uint64_t eaJobQueue,
                                                       CellSpursJobQueueHandle handle,
                                                       uint64_t eaJobHeader,
                                                       size_t sizeJobDesc, unsigned tag,
                                                       unsigned dmaTag, uint64_t eaSemaphore)
{
    return _cellSpursJobQueuePushJobBody(
        eaJobQueue, handle, eaJobHeader, sizeJobDesc, tag, dmaTag, eaSemaphore, 1, 0);
}

static inline int cellSpursJobQueuePushJobList(uint64_t eaJobQueue,
                                               CellSpursJobQueueHandle handle,
                                               uint64_t eaJobList, unsigned tag,
                                               unsigned dmaTag, uint64_t eaSemaphore)
{
    return _cellSpursJobQueuePushJobListBody(
        eaJobQueue, handle, eaJobList, tag, dmaTag, eaSemaphore, 1);
}

static inline int cellSpursJobQueueTryPushJobList(uint64_t eaJobQueue,
                                                  CellSpursJobQueueHandle handle,
                                                  uint64_t eaJobList, unsigned tag,
                                                  unsigned dmaTag, uint64_t eaSemaphore)
{
    return _cellSpursJobQueuePushJobListBody(
        eaJobQueue, handle, eaJobList, tag, dmaTag, eaSemaphore, 0);
}

static inline int cellSpursJobQueuePushFlush(uint64_t eaJobQueue,
                                             CellSpursJobQueueHandle handle,
                                             unsigned dmaTag)
{
    return _cellSpursJobQueuePushFlush(eaJobQueue, handle, dmaTag, 1);
}

static inline int cellSpursJobQueueTryPushFlush(uint64_t eaJobQueue,
                                                CellSpursJobQueueHandle handle,
                                                unsigned dmaTag)
{
    return _cellSpursJobQueuePushFlush(eaJobQueue, handle, dmaTag, 0);
}

static inline int cellSpursJobQueuePushSync(uint64_t eaJobQueue,
                                            CellSpursJobQueueHandle handle,
                                            unsigned tagMask, unsigned dmaTag)
{
    return _cellSpursJobQueuePushSync(eaJobQueue, handle, tagMask, dmaTag, 1);
}

static inline int cellSpursJobQueueTryPushSync(uint64_t eaJobQueue,
                                               CellSpursJobQueueHandle handle,
                                               unsigned tagMask, unsigned dmaTag)
{
    return _cellSpursJobQueuePushSync(eaJobQueue, handle, tagMask, dmaTag, 0);
}

extern int cellSpursJobQueueSendSignal(uint64_t eaJob);

/* -- Job-side entry points (called from cellSpursJobMain2) ---------- */

extern void cellSpursJobQueueMain(CellSpursJobContext2 *pContext,
                                  CellSpursJob256 *pJob);
extern int cellSpursJobQueueWaitSignal(uint64_t eaSuspendedJob);
extern int cellSpursJobQueueWaitSignal2(uint64_t eaSuspendedJob2,
    enum CellSpursJobQueueSuspendedJobAttribute attr);

/* -- Trace / memcheck globals (filled in by the SPU runtime) -------- */

extern unsigned int                    _gCellSpursJobQueueIsTraceEnabled;
extern unsigned int                    _gCellSpursJobQueueTraceRemainingCount;
extern _CellSpursJobQueueTracePacket  *_gCellSpursJobQueueTracePutPtr;
extern _CellSpursJobQueueTracePacket   _gCellSpursJobQueueTracePacketYield;
extern _CellSpursJobQueueTracePacket   _gCellSpursJobQueueTracePacketSleep;
extern _CellSpursJobQueueTracePacket   _gCellSpursJobQueueTracePacketResume;
extern void                            _cellSpursJobQueueTraceDump(void);

extern unsigned int                    _gCellSpursJobQueueIsMemCheckEnabled
    __attribute__((aligned(16)));
extern unsigned int                    _gCellSpursJobQueueYieldHasAuxProc
    __attribute__((aligned(16)));
extern void                            _cellSpursJobQueueHashCheck(void);
extern void                            _cellSpursJobQueueRehash(void);

#endif /* __SPU__ */

#ifdef __cplusplus
} /* extern "C" */
#endif

#include <cell/spurs/job_queue_cpp_types.h>

#endif /* __PS3DK_CELL_SPURS_JOB_QUEUE_H__ */
