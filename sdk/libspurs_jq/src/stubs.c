/* libspurs_jq stubs.c - placeholder bodies for the SPU job-queue
 * runtime entry points.
 *
 * Each entry point is declared in cell/spurs/job_queue*.h and
 * referenced from the host stub archive libspurs_jq_stub.a.  This
 * file ships return-CELL_OK / return-INVAL stubs so that an SPU
 * binary destined for a JQ workload can LINK; runtime correctness
 * is staged in incrementally over later changes.
 *
 * House rule: every stub returning CELL_SPURS_JOB_ERROR_INVAL marks
 * an unimplemented path the SPU runtime work will fill in.  The two
 * scheduler entry points (cellSpursJobMain2 + cellSpursJobQueueMain)
 * return CELL_OK so a stub-linked SPU job exits cleanly when the
 * dispatcher invokes it.
 */

#include <stdint.h>
#include <stddef.h>

#include <cell/spurs/error.h>
#include <cell/spurs/types.h>
#include <cell/spurs/job_descriptor.h>
#include <cell/spurs/job_context.h>
#include <cell/spurs/job_queue.h>
#include <cell/spurs/job_queue_port.h>
#include <cell/spurs/job_queue_port2.h>
#include <cell/spurs/job_queue_semaphore.h>

#define _STUB_OK    0
#define _STUB_INVAL CELL_SPURS_JOB_ERROR_INVAL

#define _UNUSED(x) ((void)(x))

/* -- JQ job-side entry stubs ----------------------------------------- */
/* cellSpursJobMain2 here is the JQ-aware variant: the SPRX dispatcher
 * jumps into _start of the SPU job binary, which tail-calls into this
 * function.  Real implementation calls SysCall::__initialize, runs
 * cellSpursJobQueueMain (the consumer loop), then SysCall::__finalize.
 * Stubbed to immediate return so the link succeeds. */
void cellSpursJobMain2(CellSpursJobContext2 *ctx, CellSpursJob256 *job)
{
    _UNUSED(ctx);
    _UNUSED(job);
}

void cellSpursJobQueueMain(CellSpursJobContext2 *ctx, CellSpursJob256 *job)
{
    _UNUSED(ctx);
    _UNUSED(job);
}

int cellSpursJobQueueWaitSignal(uint64_t eaSuspendedJob)
{
    _UNUSED(eaSuspendedJob);
    return _STUB_INVAL;
}

int cellSpursJobQueueWaitSignal2(uint64_t eaSuspendedJob2,
                                 enum CellSpursJobQueueSuspendedJobAttribute attr)
{
    _UNUSED(eaSuspendedJob2); _UNUSED(attr);
    return _STUB_INVAL;
}

int cellSpursJobQueueSendSignal(uint64_t eaJob)
{
    _UNUSED(eaJob);
    return _STUB_INVAL;
}

/* -- Information ----------------------------------------------------- */

uint64_t cellSpursJobQueueGetSpurs(uint64_t eaJobQueue)
{
    _UNUSED(eaJobQueue);
    return 0;
}

int cellSpursJobQueueGetHandleCount(uint64_t eaJobQueue)
{
    _UNUSED(eaJobQueue);
    return 0;
}

int cellSpursJobQueueGetError(uint64_t eaJobQueue, int *exitCode, void **cause)
{
    _UNUSED(eaJobQueue);
    if (exitCode) *exitCode = 0;
    if (cause) *cause = 0;
    return _STUB_OK;
}

int cellSpursJobQueueGetMaxSizeJobDescriptor(uint64_t eaJobQueue)
{
    _UNUSED(eaJobQueue);
    return 256;
}

int cellSpursGetJobQueueId(uint64_t eaJobQueue, CellSpursWorkloadId *pId)
{
    _UNUSED(eaJobQueue);
    if (pId) *pId = 0;
    return _STUB_OK;
}

int cellSpursJobQueueGetSuspendedJobSize(const CellSpursJobHeader *pJob,
                                         size_t sizeJobDesc,
                                         enum CellSpursJobQueueSuspendedJobAttribute attr,
                                         unsigned int *pSize)
{
    _UNUSED(pJob); _UNUSED(sizeJobDesc); _UNUSED(attr);
    if (pSize) *pSize = 0;
    return _STUB_OK;
}

/* -- Open / Close ---------------------------------------------------- */

int cellSpursJobQueueOpen(uint64_t eaJobQueue, CellSpursJobQueueHandle *handle)
{
    _UNUSED(eaJobQueue);
    if (handle) *handle = 0;
    return _STUB_OK;
}

int cellSpursJobQueueClose(uint64_t eaJobQueue, CellSpursJobQueueHandle handle)
{
    _UNUSED(eaJobQueue); _UNUSED(handle);
    return _STUB_OK;
}

/* -- JQ push body NIDs ----------------------------------------------- */

int _cellSpursJobQueueAllocateJobDescriptor(uint64_t eaJobQueue,
                                            CellSpursJobQueueHandle handle,
                                            size_t sizeJobDesc,
                                            unsigned dmaTag, unsigned flag,
                                            uint64_t *eaAllocatedJobDesc)
{
    _UNUSED(eaJobQueue); _UNUSED(handle); _UNUSED(sizeJobDesc);
    _UNUSED(dmaTag); _UNUSED(flag);
    if (eaAllocatedJobDesc) *eaAllocatedJobDesc = 0;
    return _STUB_INVAL;
}

int _cellSpursJobQueuePushAndReleaseJobBody(uint64_t eaJobQueue,
                                            CellSpursJobQueueHandle handle,
                                            uint64_t eaJobHeader, size_t sizeJobDesc,
                                            unsigned tag, unsigned dmaTag,
                                            unsigned flag, uint64_t eaSemaphore)
{
    _UNUSED(eaJobQueue); _UNUSED(handle); _UNUSED(eaJobHeader); _UNUSED(sizeJobDesc);
    _UNUSED(tag); _UNUSED(dmaTag); _UNUSED(flag); _UNUSED(eaSemaphore);
    return _STUB_INVAL;
}

int _cellSpursJobQueuePushJob2Body(uint64_t eaJobQueue,
                                   CellSpursJobQueueHandle handle,
                                   uint64_t eaJobHeader, size_t sizeJobDesc,
                                   unsigned tag, unsigned dmaTag,
                                   unsigned flag, uint64_t eaSemaphore)
{
    _UNUSED(eaJobQueue); _UNUSED(handle); _UNUSED(eaJobHeader); _UNUSED(sizeJobDesc);
    _UNUSED(tag); _UNUSED(dmaTag); _UNUSED(flag); _UNUSED(eaSemaphore);
    return _STUB_INVAL;
}

int _cellSpursJobQueuePushJobBody(uint64_t eaJobQueue,
                                  CellSpursJobQueueHandle handle,
                                  uint64_t eaJobHeader, size_t sizeJobDesc,
                                  unsigned tag, unsigned dmaTag,
                                  uint64_t eaSemaphore,
                                  unsigned isExclusive, unsigned isBlocking)
{
    _UNUSED(eaJobQueue); _UNUSED(handle); _UNUSED(eaJobHeader); _UNUSED(sizeJobDesc);
    _UNUSED(tag); _UNUSED(dmaTag); _UNUSED(eaSemaphore);
    _UNUSED(isExclusive); _UNUSED(isBlocking);
    return _STUB_INVAL;
}

int _cellSpursJobQueuePushJobListBody(uint64_t eaJobQueue,
                                      CellSpursJobQueueHandle handle,
                                      uint64_t eaJobList, unsigned tag,
                                      unsigned dmaTag, uint64_t eaSemaphore,
                                      unsigned isBlocking)
{
    _UNUSED(eaJobQueue); _UNUSED(handle); _UNUSED(eaJobList); _UNUSED(tag);
    _UNUSED(dmaTag); _UNUSED(eaSemaphore); _UNUSED(isBlocking);
    return _STUB_INVAL;
}

int _cellSpursJobQueuePushFlush(uint64_t eaJobQueue,
                                CellSpursJobQueueHandle handle,
                                unsigned dmaTag, unsigned isBlocking)
{
    _UNUSED(eaJobQueue); _UNUSED(handle); _UNUSED(dmaTag); _UNUSED(isBlocking);
    return _STUB_INVAL;
}

int _cellSpursJobQueuePushSync(uint64_t eaJobQueue,
                               CellSpursJobQueueHandle handle,
                               unsigned tagMask, unsigned dmaTag,
                               unsigned isBlocking)
{
    _UNUSED(eaJobQueue); _UNUSED(handle); _UNUSED(tagMask);
    _UNUSED(dmaTag); _UNUSED(isBlocking);
    return _STUB_INVAL;
}

/* -- Port surface --------------------------------------------------- */

uint64_t cellSpursJobQueuePortGetJobQueue(uint64_t eaPort)
{
    _UNUSED(eaPort);
    return 0;
}

int cellSpursJobQueuePortInitialize(uint64_t eaPort, uint64_t eaJobQueue,
                                    unsigned isMTSafe)
{
    _UNUSED(eaPort); _UNUSED(eaJobQueue); _UNUSED(isMTSafe);
    return _STUB_INVAL;
}

int cellSpursJobQueuePortInitializeWithDescriptorBuffer(uint64_t eaPort,
                                                        uint64_t eaJobQueue,
                                                        uint64_t eaBuffer,
                                                        size_t sizeDesc,
                                                        unsigned numEntries,
                                                        unsigned isMTSafe)
{
    _UNUSED(eaPort); _UNUSED(eaJobQueue); _UNUSED(eaBuffer);
    _UNUSED(sizeDesc); _UNUSED(numEntries); _UNUSED(isMTSafe);
    return _STUB_INVAL;
}

int cellSpursJobQueuePortFinalize(uint64_t eaPort)
{
    _UNUSED(eaPort);
    return _STUB_INVAL;
}

int _cellSpursJobQueuePortPushBody(uint64_t eaPort, uint64_t eaJobDescriptor,
                                   size_t sizeDesc, unsigned int dmaTag,
                                   unsigned int isSync, unsigned isBlocking)
{
    _UNUSED(eaPort); _UNUSED(eaJobDescriptor); _UNUSED(sizeDesc);
    _UNUSED(dmaTag); _UNUSED(isSync); _UNUSED(isBlocking);
    return _STUB_INVAL;
}

int _cellSpursJobQueuePortCopyPushBody(uint64_t eaPort,
                                       const CellSpursJobHeader *pJob,
                                       size_t sizeDesc, unsigned int dmaTag,
                                       unsigned int isSync, unsigned isBlocking)
{
    _UNUSED(eaPort); _UNUSED(pJob); _UNUSED(sizeDesc);
    _UNUSED(dmaTag); _UNUSED(isSync); _UNUSED(isBlocking);
    return _STUB_INVAL;
}

int _cellSpursJobQueuePortPushJobBody(uint64_t eaPort, uint64_t eaJobDescriptor,
                                      size_t sizeDesc, unsigned tag,
                                      unsigned int dmaTag, unsigned int isSync,
                                      unsigned isExclusive, unsigned isBlocking)
{
    _UNUSED(eaPort); _UNUSED(eaJobDescriptor); _UNUSED(sizeDesc); _UNUSED(tag);
    _UNUSED(dmaTag); _UNUSED(isSync); _UNUSED(isExclusive); _UNUSED(isBlocking);
    return _STUB_INVAL;
}

int _cellSpursJobQueuePortPushJobListBody(uint64_t eaPort, uint64_t eaJobList,
                                          unsigned tag, unsigned int dmaTag,
                                          unsigned int isSync, unsigned isBlocking)
{
    _UNUSED(eaPort); _UNUSED(eaJobList); _UNUSED(tag);
    _UNUSED(dmaTag); _UNUSED(isSync); _UNUSED(isBlocking);
    return _STUB_INVAL;
}

int _cellSpursJobQueuePortCopyPushJobBody(uint64_t eaPort,
                                          const CellSpursJobHeader *pJob,
                                          size_t sizeDesc, unsigned tag,
                                          unsigned int dmaTag, unsigned int isSync,
                                          unsigned isExclusive, unsigned isBlocking)
{
    _UNUSED(eaPort); _UNUSED(pJob); _UNUSED(sizeDesc); _UNUSED(tag);
    _UNUSED(dmaTag); _UNUSED(isSync); _UNUSED(isExclusive); _UNUSED(isBlocking);
    return _STUB_INVAL;
}

int cellSpursJobQueuePortSync(uint64_t eaPort)
{
    _UNUSED(eaPort);
    return _STUB_INVAL;
}

int cellSpursJobQueuePortTrySync(uint64_t eaPort)
{
    _UNUSED(eaPort);
    return _STUB_INVAL;
}

int _cellSpursJobQueuePortPushFlush(uint64_t eaPort, unsigned int dmaTag,
                                    unsigned isBlocking)
{
    _UNUSED(eaPort); _UNUSED(dmaTag); _UNUSED(isBlocking);
    return _STUB_INVAL;
}

int _cellSpursJobQueuePortPushSync(uint64_t eaPort, unsigned tagMask,
                                   unsigned int dmaTag, unsigned isBlocking)
{
    _UNUSED(eaPort); _UNUSED(tagMask); _UNUSED(dmaTag); _UNUSED(isBlocking);
    return _STUB_INVAL;
}

/* -- Port2 surface -------------------------------------------------- */

uint64_t cellSpursJobQueuePort2GetJobQueue(uint64_t eaPort2)
{
    _UNUSED(eaPort2);
    return 0;
}

int cellSpursJobQueuePort2Create(uint64_t eaPort2, uint64_t eaJobQueue)
{
    _UNUSED(eaPort2); _UNUSED(eaJobQueue);
    return _STUB_INVAL;
}

int cellSpursJobQueuePort2Destroy(uint64_t eaPort2)
{
    _UNUSED(eaPort2);
    return _STUB_INVAL;
}

int _cellSpursJobQueuePort2PushJobBody(uint64_t eaPort2, uint64_t eaJob,
                                       size_t sizeDesc, unsigned tag,
                                       unsigned int dmaTag, unsigned flag,
                                       unsigned isAutoRelease)
{
    _UNUSED(eaPort2); _UNUSED(eaJob); _UNUSED(sizeDesc); _UNUSED(tag);
    _UNUSED(dmaTag); _UNUSED(flag); _UNUSED(isAutoRelease);
    return _STUB_INVAL;
}

int _cellSpursJobQueuePort2PushJobListBody(uint64_t eaPort2, uint64_t eaJobList,
                                           unsigned tag, unsigned int dmaTag,
                                           unsigned flag)
{
    _UNUSED(eaPort2); _UNUSED(eaJobList); _UNUSED(tag);
    _UNUSED(dmaTag); _UNUSED(flag);
    return _STUB_INVAL;
}

int _cellSpursJobQueuePort2CopyPushJobBody(uint64_t eaPort2,
                                           const CellSpursJobHeader *pJob,
                                           size_t sizeDesc, size_t sizeDescFromPool,
                                           unsigned tag, unsigned int dmaTag,
                                           unsigned flag)
{
    _UNUSED(eaPort2); _UNUSED(pJob); _UNUSED(sizeDesc); _UNUSED(sizeDescFromPool);
    _UNUSED(tag); _UNUSED(dmaTag); _UNUSED(flag);
    return _STUB_INVAL;
}

int cellSpursJobQueuePort2AllocateJobDescriptor(uint64_t eaPort2, size_t sizeDesc,
                                                unsigned int dmaTag, unsigned flag,
                                                uint64_t *eaAllocatedJobDesc)
{
    _UNUSED(eaPort2); _UNUSED(sizeDesc); _UNUSED(dmaTag); _UNUSED(flag);
    if (eaAllocatedJobDesc) *eaAllocatedJobDesc = 0;
    return _STUB_INVAL;
}

int cellSpursJobQueuePort2Sync(uint64_t eaPort2, unsigned flag)
{
    _UNUSED(eaPort2); _UNUSED(flag);
    return _STUB_INVAL;
}

int cellSpursJobQueuePort2PushFlush(uint64_t eaPort2, unsigned flag)
{
    _UNUSED(eaPort2); _UNUSED(flag);
    return _STUB_INVAL;
}

int cellSpursJobQueuePort2PushSync(uint64_t eaPort2, unsigned tagMask, unsigned flag)
{
    _UNUSED(eaPort2); _UNUSED(tagMask); _UNUSED(flag);
    return _STUB_INVAL;
}

/* -- Semaphore ------------------------------------------------------ */

int cellSpursJobQueueSemaphoreInitialize(uint32_t eaSemaphore, uint32_t eaJobQueue)
{
    _UNUSED(eaSemaphore); _UNUSED(eaJobQueue);
    return _STUB_INVAL;
}

int cellSpursJobQueueSemaphoreAcquire(uint32_t eaSemaphore, unsigned int acquireCount)
{
    _UNUSED(eaSemaphore); _UNUSED(acquireCount);
    return _STUB_INVAL;
}

int cellSpursJobQueueSemaphoreTryAcquire(uint32_t eaSemaphore, unsigned int acquireCount)
{
    _UNUSED(eaSemaphore); _UNUSED(acquireCount);
    return _STUB_INVAL;
}

/* -- Memcheck / hash check / trace ---------------------------------- */

void _cellSpursJobQueueHashCheck(void)
{
    /* no-op */
}

void _cellSpursJobQueueRehash(void)
{
    /* no-op */
}

void _cellSpursJobQueueTraceDump(void)
{
    /* no-op */
}

/* -- Trace / memcheck globals --------------------------------------- */

unsigned int                    _gCellSpursJobQueueIsTraceEnabled        = 0;
unsigned int                    _gCellSpursJobQueueTraceRemainingCount   = 0;
_CellSpursJobQueueTracePacket  *_gCellSpursJobQueueTracePutPtr            = 0;
_CellSpursJobQueueTracePacket   _gCellSpursJobQueueTracePacketYield       = {{0,0,0,0,0},{0}};
_CellSpursJobQueueTracePacket   _gCellSpursJobQueueTracePacketSleep       = {{0,0,0,0,0},{0}};
_CellSpursJobQueueTracePacket   _gCellSpursJobQueueTracePacketResume      = {{0,0,0,0,0},{0}};

unsigned int _gCellSpursJobQueueIsMemCheckEnabled __attribute__((aligned(16))) = 0;
unsigned int _gCellSpursJobQueueYieldHasAuxProc   __attribute__((aligned(16))) = 0;
