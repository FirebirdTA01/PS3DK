/* cell/spurs/job_queue_cpp_types.h - C++ wrapper classes for the
 * SPURS job-queue API.
 *
 * Provides cell::Spurs::JobQueue::JobQueueBase (member-function
 * wrappers around the C entry points) and the
 * cell::Spurs::JobQueue::JobQueue<tDepth, tMaxSizeJobDescriptor>
 * template that bundles a JobQueueBase together with its command-list
 * storage so a single static / member object embeds the entire queue.
 *
 * SPU-side translation units that include this file see only the
 * static-constants surface; the lifecycle / push helpers (which call
 * back into PPU-side stubs) are fenced behind `#ifndef __SPU__`.
 */
#ifndef __PS3DK_CELL_SPURS_JOB_QUEUE_CPP_TYPES_H__
#define __PS3DK_CELL_SPURS_JOB_QUEUE_CPP_TYPES_H__

#include <cell/spurs/job_queue_types.h>

#ifdef __cplusplus

#ifndef __SPU__
#include <cell/sysmodule.h>
#include <assert.h>
#endif

__CELL_SPURS_JOBQUEUE_BEGIN

class JobQueueBase : public CellSpursJobQueue {
public:
    static const size_t       kAlign                 = CELL_SPURS_JOBQUEUE_ALIGN;
    static const size_t       kSize                  = CELL_SPURS_JOBQUEUE_SIZE;
    static const size_t       kDescriptorPoolAlign   = CELL_SPURS_JOBQUEUE_JOB_DESCRIPTOR_POOL_ALIGN;

    static const unsigned int kMaxDepth              = CELL_SPURS_JOBQUEUE_MAX_DEPTH;
    static const size_t       kMaxSizeJobMemory      = CELL_SPURS_JOBQUEUE_MAX_SIZE_JOB_MEMORY;
    static const int          kMaxClients            = CELL_SPURS_JOBQUEUE_MAX_CLIENTS;

    static const unsigned     kFlagExclusiveJob      = CELL_SPURS_JOBQUEUE_FLAG_EXCLUSIVE_JOB;
    static const unsigned     kFlagNonBlocking       = CELL_SPURS_JOBQUEUE_FLAG_NON_BLOCKING;

    static const uint8_t      kDefaultMaxGrab        = CELL_SPURS_JOBQUEUE_DEFAULT_MAX_GRAB;
    static const uint8_t      kMaxNumMaxGrab         = CELL_SPURS_JOBQUEUE_MAX_NUM_MAX_GRAB;
    static const uint8_t      kDefaultMaxNumJobsOnSpu = CELL_SPURS_JOBQUEUE_DEFAULT_MAX_NUM_JOBS_ON_SPU;

    static const unsigned     kMaxHandle             = CELL_SPURS_JOBQUEUE_MAX_HANDLE;
    static const unsigned     kMaxTag                = CELL_SPURS_JOBQUEUE_MAX_TAG;

    static const size_t       kWaitingJobAlign       = CELL_SPURS_JOBQUEUE_WAITING_JOB_ALIGN;
    static const size_t       kWaitingJobSize        = CELL_SPURS_JOBQUEUE_WAITING_JOB_SIZE;

#ifndef __SPU__
protected:
    JobQueueBase(const JobQueueBase&);
    JobQueueBase& operator=(const JobQueueBase&);

    static int create(JobQueueBase *pJobQueueBase,
                      CellSpurs *pSpurs,
                      const char *pName,
                      uint64_t *pCommandList,
                      unsigned int depth,
                      unsigned int numSpus,
                      const uint8_t priorityTable[CELL_SPURS_MAX_SPU],
                      unsigned int maxSizeJobDescriptor,
                      uint8_t  maxGrab,
                      bool submitWithEntryLock,
                      bool doBusyWaiting,
                      bool isHaltOnError,
                      uint8_t maxNumJobsOnASpu = CELL_SPURS_JOBQUEUE_DEFAULT_MAX_NUM_JOBS_ON_SPU,
                      bool isJobTypeMemoryCheck = false)
    {
        CellSpursJobQueueAttribute attr;
        __CELL_SPURS_RETURN_IF(cellSpursJobQueueAttributeInitialize(&attr));
        __CELL_SPURS_RETURN_IF(cellSpursJobQueueAttributeSetGrabParameters(&attr, maxNumJobsOnASpu, maxGrab));
        __CELL_SPURS_RETURN_IF(cellSpursJobQueueAttributeSetMaxSizeJobDescriptor(&attr, maxSizeJobDescriptor));
        __CELL_SPURS_RETURN_IF(cellSpursJobQueueAttributeSetSubmitWithEntryLock(&attr, submitWithEntryLock));
        __CELL_SPURS_RETURN_IF(cellSpursJobQueueAttributeSetDoBusyWaiting(&attr, doBusyWaiting));
        __CELL_SPURS_RETURN_IF(cellSpursJobQueueAttributeSetIsHaltOnError(&attr, isHaltOnError));
        __CELL_SPURS_RETURN_IF(cellSpursJobQueueAttributeSetIsJobTypeMemoryCheck(&attr, isJobTypeMemoryCheck));
        return cellSpursCreateJobQueue(pSpurs, pJobQueueBase, &attr, pName,
                                       pCommandList, depth, numSpus, priorityTable);
    }

    static int create(JobQueueBase *pJobQueueBase,
                      CellSpurs *pSpurs,
                      const char *pName,
                      uint64_t *pCommandList,
                      unsigned int depth,
                      unsigned int numSpus,
                      const uint8_t priorityTable[CELL_SPURS_MAX_SPU],
                      void *pPool,
                      const CellSpursJobQueueJobDescriptorPool *pPoolDesc,
                      unsigned int maxSizeJobDescriptor,
                      uint8_t  maxGrab,
                      bool submitWithEntryLock,
                      bool doBusyWaiting,
                      bool isHaltOnError,
                      uint8_t maxNumJobsOnASpu = CELL_SPURS_JOBQUEUE_DEFAULT_MAX_NUM_JOBS_ON_SPU,
                      bool isJobTypeMemoryCheck = false)
    {
        CellSpursJobQueueAttribute attr;
        __CELL_SPURS_RETURN_IF(cellSpursJobQueueAttributeInitialize(&attr));
        __CELL_SPURS_RETURN_IF(cellSpursJobQueueAttributeSetGrabParameters(&attr, maxNumJobsOnASpu, maxGrab));
        __CELL_SPURS_RETURN_IF(cellSpursJobQueueAttributeSetMaxSizeJobDescriptor(&attr, maxSizeJobDescriptor));
        __CELL_SPURS_RETURN_IF(cellSpursJobQueueAttributeSetSubmitWithEntryLock(&attr, submitWithEntryLock));
        __CELL_SPURS_RETURN_IF(cellSpursJobQueueAttributeSetDoBusyWaiting(&attr, doBusyWaiting));
        __CELL_SPURS_RETURN_IF(cellSpursJobQueueAttributeSetIsHaltOnError(&attr, isHaltOnError));
        __CELL_SPURS_RETURN_IF(cellSpursJobQueueAttributeSetIsJobTypeMemoryCheck(&attr, isJobTypeMemoryCheck));
        return cellSpursCreateJobQueueWithJobDescriptorPool(pSpurs, pJobQueueBase, &attr,
                                                            pPool, pPoolDesc, pName,
                                                            pCommandList, depth,
                                                            numSpus, priorityTable);
    }

    JobQueueBase() {}

public:
    int shutdown()                       { return cellSpursShutdownJobQueue(this); }
    int join(int *exitCode)              { return cellSpursJoinJobQueue(this, exitCode); }

    int allocateJobDescriptor(CellSpursJobQueueHandle handle, size_t sizeJobDesc,
                              unsigned flag, CellSpursJobHeader **eaAllocatedJobDesc)
    { return cellSpursJobQueueAllocateJobDescriptor(this, handle, sizeJobDesc, flag, eaAllocatedJobDesc); }

    int pushAndReleaseJob(CellSpursJobQueueHandle handle, CellSpursJobHeader *eaJob,
                          size_t sizeJobDesc, unsigned tag, unsigned flag,
                          CellSpursJobQueueSemaphore *eaSemaphore = 0)
    { return cellSpursJobQueuePushAndReleaseJob(this, handle, eaJob, sizeJobDesc, tag, flag, eaSemaphore); }

    int pushJob2(CellSpursJobQueueHandle handle, CellSpursJobHeader *eaJob,
                 size_t sizeJobDesc, unsigned tag, unsigned flag,
                 CellSpursJobQueueSemaphore *eaSemaphore = 0)
    { return cellSpursJobQueuePushJob2(this, handle, eaJob, sizeJobDesc, tag, flag, eaSemaphore); }

    int pushFlush(CellSpursJobQueueHandle handle)
    { return cellSpursJobQueuePushFlush(this, handle); }
    int tryPushFlush(CellSpursJobQueueHandle handle)
    { return cellSpursJobQueueTryPushFlush(this, handle); }

    int pushSync(CellSpursJobQueueHandle handle, unsigned tagMask)
    { return cellSpursJobQueuePushSync(this, handle, tagMask); }
    int tryPushSync(CellSpursJobQueueHandle handle, unsigned tagMask)
    { return cellSpursJobQueueTryPushSync(this, handle, tagMask); }

    int push(CellSpursJobQueueHandle handle, CellSpursJobHeader *pJob,
             size_t sizeJobDesc, CellSpursJobQueueSemaphore *pSemaphore = 0)
    { return cellSpursJobQueuePush(this, handle, pJob, sizeJobDesc, pSemaphore); }
    int tryPush(CellSpursJobQueueHandle handle, CellSpursJobHeader *pJob,
                size_t sizeJobDesc, CellSpursJobQueueSemaphore *pSemaphore = 0)
    { return cellSpursJobQueueTryPush(this, handle, pJob, sizeJobDesc, pSemaphore); }

    int pushJob(CellSpursJobQueueHandle handle, CellSpursJobHeader *pJob,
                size_t sizeJobDesc, unsigned tag, CellSpursJobQueueSemaphore *pSemaphore = 0)
    { return cellSpursJobQueuePushJob(this, handle, pJob, sizeJobDesc, tag, pSemaphore); }
    int tryPushJob(CellSpursJobQueueHandle handle, CellSpursJobHeader *pJob,
                   size_t sizeJobDesc, unsigned tag, CellSpursJobQueueSemaphore *pSemaphore = 0)
    { return cellSpursJobQueueTryPushJob(this, handle, pJob, sizeJobDesc, tag, pSemaphore); }

    int pushExclusiveJob(CellSpursJobQueueHandle handle, CellSpursJobHeader *pJob,
                         size_t sizeJobDesc, unsigned tag, CellSpursJobQueueSemaphore *pSemaphore = 0)
    { return cellSpursJobQueuePushExclusiveJob(this, handle, pJob, sizeJobDesc, tag, pSemaphore); }
    int tryPushExclusiveJob(CellSpursJobQueueHandle handle, CellSpursJobHeader *pJob,
                            size_t sizeJobDesc, unsigned tag, CellSpursJobQueueSemaphore *pSemaphore = 0)
    { return cellSpursJobQueueTryPushExclusiveJob(this, handle, pJob, sizeJobDesc, tag, pSemaphore); }

    int pushJobList(CellSpursJobQueueHandle handle, CellSpursJobList *pJobList,
                    unsigned tag, CellSpursJobQueueSemaphore *pSemaphore = 0)
    { return cellSpursJobQueuePushJobList(this, handle, pJobList, tag, pSemaphore); }
    int tryPushJobList(CellSpursJobQueueHandle handle, CellSpursJobList *pJobList,
                       unsigned tag, CellSpursJobQueueSemaphore *pSemaphore = 0)
    { return cellSpursJobQueueTryPushJobList(this, handle, pJobList, tag, pSemaphore); }

    /* Information */
    CellSpurs   *getSpurs()                  { return cellSpursJobQueueGetSpurs(this); }
    unsigned int getHandleCount()            { return cellSpursJobQueueGetHandleCount(this); }
    unsigned int getMaxSizeJobDescriptor()   { return cellSpursJobQueueGetMaxSizeJobDescriptor(this); }
    int          getId(CellSpursWorkloadId *pId) { return cellSpursGetJobQueueId(this, pId); }
    int          getError(int *exitCode, void **cause)
    { return cellSpursJobQueueGetError(this, exitCode, cause); }
    int          setWaitingMode(unsigned mode)
    { return cellSpursJobQueueSetWaitingMode(this, mode); }

    /* Exception handling */
    int setExceptionEventHandler(CellSpursJobQueueExceptionEventHandler handler, void *argHandler)
    { return cellSpursJobQueueSetExceptionEventHandler(this, handler, argHandler); }
    int setExceptionEventHandler2(CellSpursJobQueueExceptionEventHandler2 handler, void *argHandler)
    { return cellSpursJobQueueSetExceptionEventHandler2(this, handler, argHandler); }
    int unsetExceptionEventHandler()
    { return cellSpursJobQueueUnsetExceptionEventHandler(this); }
#endif /* __SPU__ */
};

/* JobQueue<tDepth, tMaxSizeJobDescriptor> - bundles a JobQueueBase
 * with the per-depth command-list storage so callers don't have to
 * pre-allocate it separately.  `autoPrxLoad` defaults to true and
 * loads/unloads the cellSpursJq sysmodule across create/join. */
template<unsigned int tDepth, unsigned int tMaxSizeJobDescriptor = 256>
class JobQueue : public JobQueueBase {
public:
    static const size_t kSize = CELL_SPURS_JOBQUEUE_SIZE
        + (CELL_SPURS_JOBQUEUE_SIZE_COMMAND_BUFFER(tDepth) + sizeof(uint32_t) + 127) & ~127;

private:
    uint64_t mCommandList[CELL_SPURS_JOBQUEUE_SIZE_COMMAND_BUFFER(tDepth) / sizeof(uint64_t)];
    uint32_t mAutoPrxLoad;
    uint8_t  __pad__[128 - (CELL_SPURS_JOBQUEUE_SIZE_COMMAND_BUFFER(tDepth) + sizeof(uint32_t)) % 128];

#ifndef __SPU__
public:
    static int create(JobQueue<tDepth, tMaxSizeJobDescriptor> *pJobQueue,
                      CellSpurs *pSpurs,
                      const char *pName,
                      uint8_t numSpus,
                      const uint8_t priorityTable[CELL_SPURS_MAX_SPU],
                      uint8_t  maxGrab = CELL_SPURS_JOBQUEUE_DEFAULT_MAX_GRAB,
                      bool submitWithEntryLock = false,
                      bool doBusyWaiting = false,
                      bool isHaltOnError = false,
                      bool autoPrxLoad = true,
                      uint8_t maxNumJobsOnASpu = CELL_SPURS_JOBQUEUE_DEFAULT_MAX_NUM_JOBS_ON_SPU,
                      bool isJobTypeMemoryCheck = false)
    {
        if (autoPrxLoad)
            __CELL_SPURS_RETURN_IF(cellSysmoduleLoadModule(CELL_SYSMODULE_SPURS_JQ));

        int ret = JobQueueBase::create(pJobQueue, pSpurs, pName,
                                       pJobQueue->mCommandList, (tDepth + 15) & ~15,
                                       numSpus, priorityTable,
                                       tMaxSizeJobDescriptor, maxGrab,
                                       submitWithEntryLock, doBusyWaiting,
                                       isHaltOnError, maxNumJobsOnASpu,
                                       isJobTypeMemoryCheck);
        if (ret) {
            if (autoPrxLoad) cellSysmoduleUnloadModule(CELL_SYSMODULE_SPURS_JQ);
            return ret;
        }
        pJobQueue->mAutoPrxLoad = autoPrxLoad;
        return ret;
    }

    static int create(JobQueue<tDepth, tMaxSizeJobDescriptor> *pJobQueue,
                      CellSpurs *pSpurs,
                      const char *pName,
                      uint8_t numSpus,
                      const uint8_t priorityTable[CELL_SPURS_MAX_SPU],
                      void *pPool,
                      const CellSpursJobQueueJobDescriptorPool *pPoolDesc,
                      uint8_t  maxGrab = CELL_SPURS_JOBQUEUE_DEFAULT_MAX_GRAB,
                      bool submitWithEntryLock = false,
                      bool doBusyWaiting = false,
                      bool isHaltOnError = false,
                      bool autoPrxLoad = true,
                      uint8_t maxNumJobsOnASpu = CELL_SPURS_JOBQUEUE_DEFAULT_MAX_NUM_JOBS_ON_SPU,
                      bool isJobTypeMemoryCheck = false)
    {
        if (autoPrxLoad)
            __CELL_SPURS_RETURN_IF(cellSysmoduleLoadModule(CELL_SYSMODULE_SPURS_JQ));

        int ret = JobQueueBase::create(pJobQueue, pSpurs, pName,
                                       pJobQueue->mCommandList, (tDepth + 15) & ~15,
                                       numSpus, priorityTable, pPool, pPoolDesc,
                                       tMaxSizeJobDescriptor, maxGrab,
                                       submitWithEntryLock, doBusyWaiting,
                                       isHaltOnError, maxNumJobsOnASpu,
                                       isJobTypeMemoryCheck);
        if (ret) {
            if (autoPrxLoad) cellSysmoduleUnloadModule(CELL_SYSMODULE_SPURS_JQ);
            return ret;
        }
        pJobQueue->mAutoPrxLoad = autoPrxLoad;
        return ret;
    }

    /* Per-SPU-priority scalar shorthand: replicates `priority` across
     * the priority table and forwards. */
    static int create(JobQueue<tDepth, tMaxSizeJobDescriptor> *pJobQueue,
                      CellSpurs *pSpurs,
                      const char *pName,
                      uint8_t numSpus,
                      uint8_t priority,
                      uint8_t  maxGrab = CELL_SPURS_JOBQUEUE_DEFAULT_MAX_GRAB,
                      bool submitWithEntryLock = false,
                      bool doBusyWaiting = false,
                      bool isHaltOnError = false,
                      bool autoPrxLoad = true,
                      uint8_t maxNumJobsOnASpu = CELL_SPURS_JOBQUEUE_DEFAULT_MAX_NUM_JOBS_ON_SPU,
                      bool isJobTypeMemoryCheck = false)
    {
        uint8_t priorityTable[CELL_SPURS_MAX_SPU];
        for (unsigned int i = 0; i < CELL_SPURS_MAX_SPU; ++i) priorityTable[i] = priority;
        return create(pJobQueue, pSpurs, pName, numSpus, priorityTable,
                      maxGrab, submitWithEntryLock, doBusyWaiting,
                      isHaltOnError, autoPrxLoad, maxNumJobsOnASpu,
                      isJobTypeMemoryCheck);
    }

    static int create(JobQueue<tDepth, tMaxSizeJobDescriptor> *pJobQueue,
                      CellSpurs *pSpurs,
                      const char *pName,
                      uint8_t numSpus,
                      uint8_t priority,
                      void *pPool,
                      const CellSpursJobQueueJobDescriptorPool *pPoolDesc,
                      uint8_t  maxGrab = CELL_SPURS_JOBQUEUE_DEFAULT_MAX_GRAB,
                      bool submitWithEntryLock = false,
                      bool doBusyWaiting = false,
                      bool isHaltOnError = false,
                      bool autoPrxLoad = true,
                      uint8_t maxNumJobsOnASpu = CELL_SPURS_JOBQUEUE_DEFAULT_MAX_NUM_JOBS_ON_SPU,
                      bool isJobTypeMemoryCheck = false)
    {
        uint8_t priorityTable[CELL_SPURS_MAX_SPU];
        for (unsigned int i = 0; i < CELL_SPURS_MAX_SPU; ++i) priorityTable[i] = priority;
        return create(pJobQueue, pSpurs, pName, numSpus, priorityTable,
                      pPool, pPoolDesc, maxGrab, submitWithEntryLock,
                      doBusyWaiting, isHaltOnError, autoPrxLoad,
                      maxNumJobsOnASpu, isJobTypeMemoryCheck);
    }

    int join(int *exitCode)
    {
        int ret;
        __CELL_SPURS_RETURN_IF(JobQueueBase::join(exitCode));
        if (mAutoPrxLoad) {
            ret = cellSysmoduleUnloadModule(CELL_SYSMODULE_SPURS_JQ);
            assert(ret == CELL_OK);
        }
        return CELL_OK;
    }
#endif /* __SPU__ */
};

__CELL_SPURS_JOBQUEUE_END

#endif /* __cplusplus */

#endif /* __PS3DK_CELL_SPURS_JOB_QUEUE_CPP_TYPES_H__ */
