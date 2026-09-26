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
extern int cellSpursJobQueuePort2PushFlush(uint64_t eaPort2,
                                           unsigned int dmaTag, unsigned flag);
extern int cellSpursJobQueuePort2PushSync(uint64_t eaPort2,
                                          unsigned tagMask,
                                          unsigned int dmaTag, unsigned flag);

/* -- Push inlines over the *Body entry points ------------------------
 * `flag` takes CELL_SPURS_JOBQUEUE_FLAG_*.  No descriptor pre-check is
 * done on the SPU side. */

static inline int cellSpursJobQueuePort2PushJob(uint64_t eaPort2, uint64_t eaJob,
                                                size_t sizeDesc, unsigned tag,
                                                unsigned int dmaTag, unsigned flag)
{ return _cellSpursJobQueuePort2PushJobBody(eaPort2, eaJob, sizeDesc, tag, dmaTag, flag, 0); }

static inline int cellSpursJobQueuePort2PushAndReleaseJob(uint64_t eaPort2, uint64_t eaJob,
                                                          size_t sizeDesc, unsigned tag,
                                                          unsigned int dmaTag, unsigned flag)
{ return _cellSpursJobQueuePort2PushJobBody(eaPort2, eaJob, sizeDesc, tag, dmaTag, flag, 1); }

static inline int cellSpursJobQueuePort2PushJobList(uint64_t eaPort2, uint64_t eaJobList,
                                                    unsigned tag, unsigned int dmaTag,
                                                    unsigned flag)
{ return _cellSpursJobQueuePort2PushJobListBody(eaPort2, eaJobList, tag, dmaTag, flag); }

static inline int cellSpursJobQueuePort2CopyPushJob(uint64_t eaPort2,
                                                    const CellSpursJobHeader *pJob,
                                                    size_t sizeDesc,
                                                    size_t sizeDescFromPool,
                                                    unsigned tag, unsigned int dmaTag,
                                                    unsigned flag)
{
    return _cellSpursJobQueuePort2CopyPushJobBody(eaPort2, pJob, sizeDesc,
                                                  sizeDescFromPool, tag, dmaTag, flag);
}

#endif /* __SPU__ */

#ifdef __cplusplus
} /* extern "C" */
#endif

#include <cell/spurs/job_queue_port2_cpp_types.h>

#if defined(__cplusplus) && defined(__SPU__)

__CELL_SPURS_JOBQUEUE_BEGIN

/* SPU handle on a Port2 in main memory: holds the port's EA (set with
 * setObject) and forwards to the EA-based Port2 API.  Not copyable. */
class Port2Stub {
protected:
    uint64_t mEaPort2;

private:
    Port2Stub(const Port2Stub &);
    Port2Stub &operator=(const Port2Stub &);

public:
    static const unsigned kFlagSyncJob      = CELL_SPURS_JOBQUEUE_FLAG_SYNC_JOB;
    static const unsigned kFlagExclusiveJob = CELL_SPURS_JOBQUEUE_FLAG_EXCLUSIVE_JOB;
    static const unsigned kFlagNonBlocking  = CELL_SPURS_JOBQUEUE_FLAG_NON_BLOCKING;

    Port2Stub() : mEaPort2(0) {}
    ~Port2Stub() {}

    void setObject(uint64_t eaPort2) { mEaPort2 = eaPort2; }
    uint64_t getObject() { return mEaPort2; }

    int create(uint64_t eaJobQueue, void *reserved = 0)
    {
        (void)reserved;
        return cellSpursJobQueuePort2Create(mEaPort2, eaJobQueue);
    }
    int destroy()
    { return cellSpursJobQueuePort2Destroy(mEaPort2); }
    uint64_t getJobQueue()
    { return cellSpursJobQueuePort2GetJobQueue(mEaPort2); }

    int pushJob(uint64_t eaJob, size_t sizeDesc, unsigned tag,
                unsigned int dmaTag, unsigned flag)
    { return cellSpursJobQueuePort2PushJob(mEaPort2, eaJob, sizeDesc, tag, dmaTag, flag); }
    int pushAndReleaseJob(uint64_t eaJob, size_t sizeDesc, unsigned tag,
                          unsigned int dmaTag, unsigned flag)
    { return cellSpursJobQueuePort2PushAndReleaseJob(mEaPort2, eaJob, sizeDesc, tag, dmaTag, flag); }
    int pushJobList(uint64_t eaJobList, unsigned tag,
                    unsigned int dmaTag, unsigned flag)
    { return cellSpursJobQueuePort2PushJobList(mEaPort2, eaJobList, tag, dmaTag, flag); }
    int copyPushJob(const CellSpursJobHeader *pJob, size_t sizeDesc,
                    size_t sizeDescFromPool, unsigned tag,
                    unsigned int dmaTag, unsigned flag)
    {
        return cellSpursJobQueuePort2CopyPushJob(mEaPort2, pJob, sizeDesc,
                                                 sizeDescFromPool, tag, dmaTag, flag);
    }
    int allocateJobDescriptor(size_t sizeDesc, unsigned int dmaTag,
                              unsigned flag, uint64_t *eaAllocatedJobDesc)
    {
        return cellSpursJobQueuePort2AllocateJobDescriptor(mEaPort2, sizeDesc, dmaTag,
                                                           flag, eaAllocatedJobDesc);
    }

    int sync(unsigned flag)
    { return cellSpursJobQueuePort2Sync(mEaPort2, flag); }
    int pushFlush(unsigned int dmaTag, unsigned flag)
    { return cellSpursJobQueuePort2PushFlush(mEaPort2, dmaTag, flag); }
    int pushSync(unsigned tagMask, unsigned int dmaTag, unsigned flag)
    { return cellSpursJobQueuePort2PushSync(mEaPort2, tagMask, dmaTag, flag); }
};

__CELL_SPURS_JOBQUEUE_END

#endif /* __cplusplus && __SPU__ */

#endif /* __PS3DK_CELL_SPURS_JOB_QUEUE_PORT2_H__ */
