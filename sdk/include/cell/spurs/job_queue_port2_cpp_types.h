/* cell/spurs/job_queue_port2_cpp_types.h - C++ wrapper for the
 * CellSpursJobQueuePort2 C surface.
 */
#ifndef __PS3DK_CELL_SPURS_JOB_QUEUE_PORT2_CPP_TYPES_H__
#define __PS3DK_CELL_SPURS_JOB_QUEUE_PORT2_CPP_TYPES_H__

#include <cell/spurs/job_queue_port2_types.h>
#include <cell/spurs/job_queue_types.h>

#ifdef __cplusplus

__CELL_SPURS_JOBQUEUE_BEGIN

class Port2 : public CellSpursJobQueuePort2 {
#ifndef __SPU__
protected:
    Port2(const Port2&);
    Port2& operator=(const Port2&);
#endif

public:
    static const size_t   kAlign = CELL_SPURS_JOBQUEUE_PORT2_ALIGN;
    static const size_t   kSize  = CELL_SPURS_JOBQUEUE_PORT2_SIZE;

    static const unsigned kFlagSyncJob      = CELL_SPURS_JOBQUEUE_FLAG_SYNC_JOB;
    static const unsigned kFlagExclusiveJob = CELL_SPURS_JOBQUEUE_FLAG_EXCLUSIVE_JOB;
    static const unsigned kFlagNonBlocking  = CELL_SPURS_JOBQUEUE_FLAG_NON_BLOCKING;

#ifndef __SPU__
    Port2()  {}
    ~Port2() {}

    static int create(Port2 *eaPort2, CellSpursJobQueue *eaJobQueue)
    { return cellSpursJobQueuePort2Create(eaPort2, eaJobQueue); }

    int destroy()                       { return cellSpursJobQueuePort2Destroy(this); }
    CellSpursJobQueue *getJobQueue()    { return cellSpursJobQueuePort2GetJobQueue(this); }

    int pushJob(CellSpursJobHeader *eaJob, size_t sizeDesc, unsigned tag, unsigned flag)
    {
#ifdef CELL_SPURS_JOBDESCRIPTOR_ERROR_CHECK
        __CELL_SPURS_RETURN_IF(cellSpursJobQueueCheckJob(
            (const CellSpursJob256 *)(uintptr_t)eaJob, sizeDesc,
            cellSpursJobQueueGetMaxSizeJobDescriptor(cellSpursJobQueuePort2GetJobQueue(this))));
#endif
        return _cellSpursJobQueuePort2PushJobBody(this, eaJob, sizeDesc, tag, flag);
    }

    int pushJobList(CellSpursJobList *eaJobList, unsigned tag, unsigned flag)
    {
#ifdef CELL_SPURS_JOBDESCRIPTOR_ERROR_CHECK
        for (unsigned int i = 0; i < eaJobList->numJobs; ++i) {
            CellSpursJob256 *eaJob = (CellSpursJob256 *)((uintptr_t)eaJobList->eaJobList
                                                          + eaJobList->sizeOfJob * i);
            __CELL_SPURS_RETURN_IF(cellSpursJobQueueCheckJob(
                (const CellSpursJob256 *)eaJob, eaJobList->sizeOfJob,
                cellSpursJobQueueGetMaxSizeJobDescriptor(cellSpursJobQueuePort2GetJobQueue(this))));
        }
#endif
        return _cellSpursJobQueuePort2PushJobListBody(this, eaJobList, tag, flag);
    }

    int copyPushJob(const CellSpursJobHeader *pJob, size_t sizeDesc,
                    size_t sizeDescFromPool, unsigned tag, unsigned flag)
    {
#ifdef CELL_SPURS_JOBDESCRIPTOR_ERROR_CHECK
        __CELL_SPURS_RETURN_IF(cellSpursJobQueueCheckJob(
            (const CellSpursJob256 *)(uintptr_t)pJob, sizeDesc,
            cellSpursJobQueueGetMaxSizeJobDescriptor(cellSpursJobQueuePort2GetJobQueue(this))));
#endif
        return _cellSpursJobQueuePort2CopyPushJobBody(this, pJob, sizeDesc, sizeDescFromPool, tag, flag);
    }

    int pushAndReleaseJob(CellSpursJobHeader *eaJob, size_t sizeDesc,
                          unsigned tag, unsigned flag)
    {
#ifdef CELL_SPURS_JOBDESCRIPTOR_ERROR_CHECK
        __CELL_SPURS_RETURN_IF(cellSpursJobQueueCheckJob(
            (const CellSpursJob256 *)(uintptr_t)eaJob, sizeDesc,
            cellSpursJobQueueGetMaxSizeJobDescriptor(cellSpursJobQueuePort2GetJobQueue(this))));
#endif
        return _cellSpursJobQueuePort2PushAndReleaseJobBody(this, eaJob, sizeDesc, tag, flag);
    }

    int allocateJobDescriptor(size_t sizeDesc, unsigned flag,
                              CellSpursJobHeader **eaAllocatedJobDesc)
    { return cellSpursJobQueuePort2AllocateJobDescriptor(this, sizeDesc, flag, eaAllocatedJobDesc); }

    int sync(unsigned flag)                       { return cellSpursJobQueuePort2Sync(this, flag); }
    int pushFlush(unsigned flag)                  { return cellSpursJobQueuePort2PushFlush(this, flag); }
    int pushSync(unsigned tagMask, unsigned flag) { return cellSpursJobQueuePort2PushSync(this, tagMask, flag); }
#endif /* __SPU__ */
};

__CELL_SPURS_JOBQUEUE_END

#endif /* __cplusplus */

#endif /* __PS3DK_CELL_SPURS_JOB_QUEUE_PORT2_CPP_TYPES_H__ */
