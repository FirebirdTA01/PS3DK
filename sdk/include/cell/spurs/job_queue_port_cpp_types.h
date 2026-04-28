/* cell/spurs/job_queue_port_cpp_types.h - C++ wrappers for the
 * CellSpursJobQueuePort C surface.
 *
 * Provides cell::Spurs::JobQueue::Port (member-function wrappers)
 * and cell::Spurs::JobQueue::PortWithDescriptorBuffer<JobT, N> which
 * bundles the port together with a fixed JobT[N] descriptor buffer
 * for the WithDescriptorBuffer Initialize variant.
 */
#ifndef __PS3DK_CELL_SPURS_JOB_QUEUE_PORT_CPP_TYPES_H__
#define __PS3DK_CELL_SPURS_JOB_QUEUE_PORT_CPP_TYPES_H__

#include <cell/spurs/job_queue_port_types.h>
#include <cell/spurs/job_queue_types.h>

#ifdef __cplusplus

__CELL_SPURS_JOBQUEUE_BEGIN

class Port : public CellSpursJobQueuePort {
#ifndef __SPU__
protected:
    Port(const Port&);
    Port& operator=(const Port&);
#endif

public:
    static const size_t kAlign = CELL_SPURS_JOBQUEUE_PORT_ALIGN;
    static const size_t kSize  = CELL_SPURS_JOBQUEUE_PORT_SIZE;

#ifndef __SPU__
    Port()  {}
    ~Port() {}

    int initialize(CellSpursJobQueue *pJobQueue, bool isMTSafe = true)
    { return cellSpursJobQueuePortInitialize(this, pJobQueue, isMTSafe); }
    int finalize()
    { return cellSpursJobQueuePortFinalize(this); }

    CellSpursJobQueue *getJobQueue()
    { return cellSpursJobQueuePortGetJobQueue(this); }

    int push(CellSpursJobHeader *pJob, size_t sizeJobDesc, bool isSync)
    { return cellSpursJobQueuePortPush(this, pJob, sizeJobDesc, isSync); }
    int copyPush(const CellSpursJobHeader *pJob, size_t sizeJobDesc, bool isSync)
    { return cellSpursJobQueuePortCopyPush(this, pJob, sizeJobDesc, isSync); }

    int pushJob(CellSpursJobHeader *pJob, size_t sizeJobDesc, unsigned tag, bool isSync)
    { return cellSpursJobQueuePortPushJob(this, pJob, sizeJobDesc, tag, isSync); }
    int pushExclusiveJob(CellSpursJobHeader *pJob, size_t sizeJobDesc, unsigned tag, bool isSync)
    { return cellSpursJobQueuePortPushExclusiveJob(this, pJob, sizeJobDesc, tag, isSync); }
    int pushJobList(CellSpursJobList *pJobList, unsigned tag, bool isSync)
    { return cellSpursJobQueuePortPushJobList(this, pJobList, tag, isSync); }
    int copyPushJob(const CellSpursJobHeader *pJob, size_t sizeJobDesc, unsigned tag, bool isSync)
    { return cellSpursJobQueuePortCopyPushJob(this, pJob, sizeJobDesc, tag, isSync); }
    int copyPushExclusiveJob(const CellSpursJobHeader *pJob, size_t sizeJobDesc, unsigned tag, bool isSync)
    { return cellSpursJobQueuePortCopyPushExclusiveJob(this, pJob, sizeJobDesc, tag, isSync); }

    int sync()                          { return cellSpursJobQueuePortSync(this); }
    int pushFlush()                     { return cellSpursJobQueuePortPushFlush(this); }
    int pushSync(unsigned tagMask)      { return cellSpursJobQueuePortPushSync(this, tagMask); }

    int tryPush(CellSpursJobHeader *pJob, size_t sizeJobDesc, bool isSync)
    { return cellSpursJobQueuePortTryPush(this, pJob, sizeJobDesc, isSync); }
    int tryCopyPush(const CellSpursJobHeader *pJob, size_t sizeJobDesc, bool isSync)
    { return cellSpursJobQueuePortTryCopyPush(this, pJob, sizeJobDesc, isSync); }
    int tryPushJob(CellSpursJobHeader *pJob, size_t sizeJobDesc, unsigned tag, bool isSync)
    { return cellSpursJobQueuePortTryPushJob(this, pJob, sizeJobDesc, tag, isSync); }
    int tryPushExclusiveJob(CellSpursJobHeader *pJob, size_t sizeJobDesc, unsigned tag, bool isSync)
    { return cellSpursJobQueuePortTryPushExclusiveJob(this, pJob, sizeJobDesc, tag, isSync); }
    int tryPushJobList(CellSpursJobList *pJobList, unsigned tag, bool isSync)
    { return cellSpursJobQueuePortTryPushJobList(this, pJobList, tag, isSync); }
    int tryCopyPushJob(const CellSpursJobHeader *pJob, size_t sizeJobDesc, unsigned tag, bool isSync)
    { return cellSpursJobQueuePortTryCopyPushJob(this, pJob, sizeJobDesc, tag, isSync); }
    int tryCopyPushExclusiveJob(const CellSpursJobHeader *pJob, size_t sizeJobDesc, unsigned tag, bool isSync)
    { return cellSpursJobQueuePortTryCopyPushExclusiveJob(this, pJob, sizeJobDesc, tag, isSync); }

    int trySync()                       { return cellSpursJobQueuePortTrySync(this); }
    int tryPushFlush()                  { return cellSpursJobQueuePortTryPushFlush(this); }
    int tryPushSync(unsigned tagMask)   { return cellSpursJobQueuePortTryPushSync(this, tagMask); }
#endif /* __SPU__ */
};

template<typename JobType, int numEntries>
class PortWithDescriptorBuffer : public Port {
private:
    JobType mDescriptorBuffer[numEntries];

#ifndef __SPU__
public:
    int initialize(CellSpursJobQueue *pJobQueue, bool isMTSafe = true)
    {
        return cellSpursJobQueuePortInitializeWithDescriptorBuffer(
            this, pJobQueue,
            (CellSpursJobHeader *)mDescriptorBuffer,
            sizeof(JobType), numEntries, isMTSafe);
    }

    int copyPush(const JobType *job, size_t sizeJobDesc, bool isSync)
    { return Port::copyPush((const CellSpursJobHeader *)job, sizeJobDesc, isSync); }
    int copyPushJob(const JobType *job, size_t sizeJobDesc, unsigned tag, bool isSync)
    { return Port::copyPushJob((const CellSpursJobHeader *)job, sizeJobDesc, tag, isSync); }
    int tryCopyPush(const JobType *job, size_t sizeJobDesc, bool isSync)
    { return Port::tryCopyPush((const CellSpursJobHeader *)job, sizeJobDesc, isSync); }
    int tryCopyPushJob(const JobType *job, size_t sizeJobDesc, unsigned tag, bool isSync)
    { return Port::tryCopyPushJob((const CellSpursJobHeader *)job, sizeJobDesc, tag, isSync); }
#endif /* __SPU__ */
};

__CELL_SPURS_JOBQUEUE_END

#endif /* __cplusplus */

#endif /* __PS3DK_CELL_SPURS_JOB_QUEUE_PORT_CPP_TYPES_H__ */
