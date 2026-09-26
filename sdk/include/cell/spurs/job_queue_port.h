/* cell/spurs/job_queue_port.h - SPURS job-queue PPU port surface.
 *
 * A port is a per-thread submission handle bound to a job queue.
 * Threads that push jobs through the same port serialise relative to
 * each other; threads pushing through different ports race
 * independently.  Use isMTSafe=true on Initialize to keep a single
 * port shared by multiple threads.
 *
 * The port API supports four push patterns:
 *   - Push  / TryPush       (zero-copy: takes ownership of the descriptor)
 *   - PushJob / PushExclusiveJob (zero-copy with tag, optional exclusivity)
 *   - CopyPush / CopyPushJob   (descriptor is copied internally)
 *   - PushJobList              (batch push of a CellSpursJobList)
 * Each has a Try* variant that returns CELL_SPURS_JOB_ERROR_BUSY
 * instead of blocking when the queue is full.
 *
 * Sync flushes a port's pending pushes; PushSync waits for the queue
 * to drain past a tag mask.  Final teardown is Finalize.
 */
#ifndef __PS3DK_CELL_SPURS_JOB_QUEUE_PORT_H__
#define __PS3DK_CELL_SPURS_JOB_QUEUE_PORT_H__

#include <stdint.h>
#include <stddef.h>

#include <cell/spurs/job_descriptor.h>
#include <cell/spurs/job_queue.h>
#include <cell/spurs/job_queue_port_types.h>

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

extern CellSpursJobQueue *cellSpursJobQueuePortGetJobQueue(
    const CellSpursJobQueuePort *pPort);

/* -- Lifecycle ------------------------------------------------------- */

extern int cellSpursJobQueuePortInitialize(CellSpursJobQueuePort *pPort,
                                           CellSpursJobQueue *pJobQueue,
                                           bool isMTSafe);
extern int cellSpursJobQueuePortInitializeWithDescriptorBuffer(
    CellSpursJobQueuePort *pPort,
    CellSpursJobQueue *pJobQueue,
    CellSpursJobHeader *buffer,
    size_t sizeDescriptor,
    unsigned numEntries,
    bool isMTSafe);
extern int cellSpursJobQueuePortFinalize(CellSpursJobQueuePort *pPort);

/* -- Push body NIDs (used by the inline wrappers below) ------------- */

extern int _cellSpursJobQueuePortPushBody(CellSpursJobQueuePort *pPort,
                                          CellSpursJobHeader *pJob,
                                          size_t sizeJobDesc,
                                          bool isSync, bool isBlocking);

extern int _cellSpursJobQueuePortCopyPushBody(CellSpursJobQueuePort *pPort,
                                              const CellSpursJobHeader *pJob,
                                              size_t sizeJobDesc,
                                              bool isSync, bool isBlocking);

extern int _cellSpursJobQueuePortPushJobBody(CellSpursJobQueuePort *pPort,
                                             CellSpursJobHeader *pJob,
                                             size_t sizeJobDesc,
                                             unsigned tag,
                                             bool isSync, bool isBlocking);

extern int _cellSpursJobQueuePortPushJobBody2(CellSpursJobQueuePort *pPort,
                                              CellSpursJobHeader *pJob,
                                              size_t sizeJobDesc,
                                              unsigned tag,
                                              bool isSync,
                                              bool isExclusive,
                                              bool isBlocking);

extern int _cellSpursJobQueuePortCopyPushJobBody(CellSpursJobQueuePort *pPort,
                                                 const CellSpursJobHeader *pJob,
                                                 size_t sizeJobDesc,
                                                 unsigned tag,
                                                 bool isSync, bool isBlocking);

extern int _cellSpursJobQueuePortCopyPushJobBody2(CellSpursJobQueuePort *pPort,
                                                  const CellSpursJobHeader *pJob,
                                                  size_t sizeJobDesc,
                                                  unsigned tag,
                                                  bool isSync,
                                                  bool isExclusive,
                                                  bool isBlocking);

extern int _cellSpursJobQueuePortPushJobListBody(CellSpursJobQueuePort *pPort,
                                                 CellSpursJobList *pJobList,
                                                 unsigned tag,
                                                 bool isSync, bool isBlocking);

/* -- External push-side inline wrappers ------------------------------ */

#ifdef CELL_SPURS_JOBDESCRIPTOR_ERROR_CHECK
#  define _PS3DK_JQ_PORT_CHECK(pPort, pJob, sizeJobDesc)                     \
    __CELL_SPURS_RETURN_IF(cellSpursJobQueueCheckJob(                        \
        (const CellSpursJob256 *)(uintptr_t)(pJob), (sizeJobDesc),           \
        cellSpursJobQueueGetMaxSizeJobDescriptor(                            \
            cellSpursJobQueuePortGetJobQueue(pPort))))
#else
#  define _PS3DK_JQ_PORT_CHECK(pPort, pJob, sizeJobDesc) ((void)0)
#endif

static inline int cellSpursJobQueuePortPush(CellSpursJobQueuePort *pPort,
                                            CellSpursJobHeader *pJob,
                                            size_t sizeJobDesc, bool isSync)
{
    _PS3DK_JQ_PORT_CHECK(pPort, pJob, sizeJobDesc);
    return _cellSpursJobQueuePortPushBody(pPort, pJob, sizeJobDesc, isSync, true);
}

static inline int cellSpursJobQueuePortTryPush(CellSpursJobQueuePort *pPort,
                                               CellSpursJobHeader *pJob,
                                               size_t sizeJobDesc, bool isSync)
{
    _PS3DK_JQ_PORT_CHECK(pPort, pJob, sizeJobDesc);
    return _cellSpursJobQueuePortPushBody(pPort, pJob, sizeJobDesc, isSync, false);
}

static inline int cellSpursJobQueuePortCopyPush(CellSpursJobQueuePort *pPort,
                                                const CellSpursJobHeader *pJob,
                                                size_t sizeJobDesc, bool isSync)
{
    _PS3DK_JQ_PORT_CHECK(pPort, pJob, sizeJobDesc);
    return _cellSpursJobQueuePortCopyPushBody(pPort, pJob, sizeJobDesc, isSync, true);
}

static inline int cellSpursJobQueuePortTryCopyPush(CellSpursJobQueuePort *pPort,
                                                   const CellSpursJobHeader *pJob,
                                                   size_t sizeJobDesc, bool isSync)
{
    _PS3DK_JQ_PORT_CHECK(pPort, pJob, sizeJobDesc);
    return _cellSpursJobQueuePortCopyPushBody(pPort, pJob, sizeJobDesc, isSync, false);
}

static inline int cellSpursJobQueuePortPushJob(CellSpursJobQueuePort *pPort,
                                               CellSpursJobHeader *pJob,
                                               size_t sizeJobDesc,
                                               unsigned tag, bool isSync)
{
    _PS3DK_JQ_PORT_CHECK(pPort, pJob, sizeJobDesc);
    return _cellSpursJobQueuePortPushJobBody2(pPort, pJob, sizeJobDesc, tag,
                                              isSync, false, true);
}

static inline int cellSpursJobQueuePortPushExclusiveJob(CellSpursJobQueuePort *pPort,
                                                        CellSpursJobHeader *pJob,
                                                        size_t sizeJobDesc,
                                                        unsigned tag, bool isSync)
{
    _PS3DK_JQ_PORT_CHECK(pPort, pJob, sizeJobDesc);
    return _cellSpursJobQueuePortPushJobBody2(pPort, pJob, sizeJobDesc, tag,
                                              isSync, true, true);
}

static inline int cellSpursJobQueuePortTryPushJob(CellSpursJobQueuePort *pPort,
                                                  CellSpursJobHeader *pJob,
                                                  size_t sizeJobDesc,
                                                  unsigned tag, bool isSync)
{
    _PS3DK_JQ_PORT_CHECK(pPort, pJob, sizeJobDesc);
    return _cellSpursJobQueuePortPushJobBody2(pPort, pJob, sizeJobDesc, tag,
                                              isSync, false, false);
}

static inline int cellSpursJobQueuePortTryPushExclusiveJob(CellSpursJobQueuePort *pPort,
                                                           CellSpursJobHeader *pJob,
                                                           size_t sizeJobDesc,
                                                           unsigned tag, bool isSync)
{
    _PS3DK_JQ_PORT_CHECK(pPort, pJob, sizeJobDesc);
    return _cellSpursJobQueuePortPushJobBody2(pPort, pJob, sizeJobDesc, tag,
                                              isSync, true, false);
}

static inline int cellSpursJobQueuePortCopyPushJob(CellSpursJobQueuePort *pPort,
                                                   const CellSpursJobHeader *pJob,
                                                   size_t sizeJobDesc,
                                                   unsigned tag, bool isSync)
{
    _PS3DK_JQ_PORT_CHECK(pPort, pJob, sizeJobDesc);
    return _cellSpursJobQueuePortCopyPushJobBody2(pPort, pJob, sizeJobDesc, tag,
                                                  isSync, false, true);
}

static inline int cellSpursJobQueuePortCopyPushExclusiveJob(CellSpursJobQueuePort *pPort,
                                                            const CellSpursJobHeader *pJob,
                                                            size_t sizeJobDesc,
                                                            unsigned tag, bool isSync)
{
    _PS3DK_JQ_PORT_CHECK(pPort, pJob, sizeJobDesc);
    return _cellSpursJobQueuePortCopyPushJobBody2(pPort, pJob, sizeJobDesc, tag,
                                                  isSync, true, true);
}

static inline int cellSpursJobQueuePortTryCopyPushJob(CellSpursJobQueuePort *pPort,
                                                      const CellSpursJobHeader *pJob,
                                                      size_t sizeJobDesc,
                                                      unsigned tag, bool isSync)
{
    _PS3DK_JQ_PORT_CHECK(pPort, pJob, sizeJobDesc);
    return _cellSpursJobQueuePortCopyPushJobBody2(pPort, pJob, sizeJobDesc, tag,
                                                  isSync, false, false);
}

static inline int cellSpursJobQueuePortTryCopyPushExclusiveJob(CellSpursJobQueuePort *pPort,
                                                               const CellSpursJobHeader *pJob,
                                                               size_t sizeJobDesc,
                                                               unsigned tag, bool isSync)
{
    _PS3DK_JQ_PORT_CHECK(pPort, pJob, sizeJobDesc);
    return _cellSpursJobQueuePortCopyPushJobBody2(pPort, pJob, sizeJobDesc, tag,
                                                  isSync, true, false);
}

static inline int cellSpursJobQueuePortPushJobList(CellSpursJobQueuePort *pPort,
                                                   CellSpursJobList *pJobList,
                                                   unsigned tag, bool isSync)
{
#ifdef CELL_SPURS_JOBDESCRIPTOR_ERROR_CHECK
    for (unsigned int i = 0; i < pJobList->numJobs; ++i) {
        CellSpursJob256 *pJob = (CellSpursJob256 *)((uintptr_t)pJobList->eaJobList
                                                     + pJobList->sizeOfJob * i);
        __CELL_SPURS_RETURN_IF(cellSpursJobQueueCheckJob(
            (const CellSpursJob256 *)pJob, pJobList->sizeOfJob,
            cellSpursJobQueueGetMaxSizeJobDescriptor(
                cellSpursJobQueuePortGetJobQueue(pPort))));
    }
#endif
    return _cellSpursJobQueuePortPushJobListBody(pPort, pJobList, tag, isSync, true);
}

static inline int cellSpursJobQueuePortTryPushJobList(CellSpursJobQueuePort *pPort,
                                                      CellSpursJobList *pJobList,
                                                      unsigned tag, bool isSync)
{
#ifdef CELL_SPURS_JOBDESCRIPTOR_ERROR_CHECK
    for (unsigned int i = 0; i < pJobList->numJobs; ++i) {
        CellSpursJob256 *pJob = (CellSpursJob256 *)((uintptr_t)pJobList->eaJobList
                                                     + pJobList->sizeOfJob * i);
        __CELL_SPURS_RETURN_IF(cellSpursJobQueueCheckJob(
            (const CellSpursJob256 *)pJob, pJobList->sizeOfJob,
            cellSpursJobQueueGetMaxSizeJobDescriptor(
                cellSpursJobQueuePortGetJobQueue(pPort))));
    }
#endif
    return _cellSpursJobQueuePortPushJobListBody(pPort, pJobList, tag, isSync, false);
}

/* -- Sync ------------------------------------------------------------ */

extern int cellSpursJobQueuePortSync(CellSpursJobQueuePort *pPort);
extern int cellSpursJobQueuePortTrySync(CellSpursJobQueuePort *pPort);

extern int _cellSpursJobQueuePortPushFlush(CellSpursJobQueuePort *pPort,
                                           bool isBlocking);
static inline int cellSpursJobQueuePortPushFlush(CellSpursJobQueuePort *pPort)
{
    return _cellSpursJobQueuePortPushFlush(pPort, true);
}
static inline int cellSpursJobQueuePortTryPushFlush(CellSpursJobQueuePort *pPort)
{
    return _cellSpursJobQueuePortPushFlush(pPort, false);
}

extern int _cellSpursJobQueuePortPushSync(CellSpursJobQueuePort *pPort,
                                          unsigned tagMask, bool isBlocking);
static inline int cellSpursJobQueuePortPushSync(CellSpursJobQueuePort *pPort,
                                                unsigned tagMask)
{
    return _cellSpursJobQueuePortPushSync(pPort, tagMask, true);
}
static inline int cellSpursJobQueuePortTryPushSync(CellSpursJobQueuePort *pPort,
                                                   unsigned tag)
{
    return _cellSpursJobQueuePortPushSync(pPort, tag, false);
}

#else /* __SPU__ */
/* =====================================================================
 * SPU surface
 *
 * SPU port API takes 64-bit EAs (uint64_t) for ports / queues / job
 * descriptors and a per-call dmaTag.  The inline DMA-yield helpers
 * (which depend on cell/dma.h) are documented in the libspurs_jq SPU
 * runtime; this header carries the entry-point declarations only.
 * =====================================================================*/

extern uint64_t cellSpursJobQueuePortGetJobQueue(uint64_t eaPort);

extern int cellSpursJobQueuePortInitialize(uint64_t eaPort,
                                           uint64_t eaJobQueue,
                                           unsigned isMTSafe);
extern int cellSpursJobQueuePortInitializeWithDescriptorBuffer(
    uint64_t eaPort, uint64_t eaJobQueue, uint64_t eaBuffer,
    size_t sizeDesc, unsigned numEntries, unsigned isMTSafe);
extern int cellSpursJobQueuePortFinalize(uint64_t eaPort);

extern int _cellSpursJobQueuePortPushBody(uint64_t eaPort,
                                          uint64_t eaJobDescriptor,
                                          size_t sizeDesc,
                                          unsigned int dmaTag,
                                          unsigned int isSync,
                                          unsigned isBlocking);

extern int _cellSpursJobQueuePortCopyPushBody(uint64_t eaPort,
                                              const CellSpursJobHeader *pJob,
                                              size_t sizeDesc,
                                              unsigned int dmaTag,
                                              unsigned int isSync,
                                              unsigned isBlocking);

extern int _cellSpursJobQueuePortPushJobBody(uint64_t eaPort,
                                             uint64_t eaJobDescriptor,
                                             size_t sizeDesc, unsigned tag,
                                             unsigned int dmaTag,
                                             unsigned int isSync,
                                             unsigned isExclusive,
                                             unsigned isBlocking);

extern int _cellSpursJobQueuePortPushJobListBody(uint64_t eaPort,
                                                 uint64_t eaJobList,
                                                 unsigned tag,
                                                 unsigned int dmaTag,
                                                 unsigned int isSync,
                                                 unsigned isBlocking);

extern int _cellSpursJobQueuePortCopyPushJobBody(uint64_t eaPort,
                                                 const CellSpursJobHeader *pJob,
                                                 size_t sizeDesc, unsigned tag,
                                                 unsigned int dmaTag,
                                                 unsigned int isSync,
                                                 unsigned isExclusive,
                                                 unsigned isBlocking);

extern int cellSpursJobQueuePortSync(uint64_t eaPort);
extern int cellSpursJobQueuePortTrySync(uint64_t eaPort);

extern int _cellSpursJobQueuePortPushFlush(uint64_t eaPort,
                                           unsigned int dmaTag,
                                           unsigned isBlocking);
extern int _cellSpursJobQueuePortPushSync(uint64_t eaPort,
                                          unsigned tagMask,
                                          unsigned int dmaTag,
                                          unsigned isBlocking);

/* -- Push inlines over the *Body entry points ------------------------
 * The Try* forms return instead of waiting for queue space.  No
 * descriptor pre-check is done on the SPU side. */

static inline int cellSpursJobQueuePortPush(uint64_t eaPort,
                                            uint64_t eaJobDescriptor,
                                            size_t sizeDesc,
                                            unsigned int dmaTag,
                                            unsigned int isSync)
{ return _cellSpursJobQueuePortPushBody(eaPort, eaJobDescriptor, sizeDesc, dmaTag, isSync, 1); }

static inline int cellSpursJobQueuePortPushJob(uint64_t eaPort,
                                               uint64_t eaJobDescriptor,
                                               size_t sizeDesc, unsigned tag,
                                               unsigned int dmaTag,
                                               unsigned int isSync)
{ return _cellSpursJobQueuePortPushJobBody(eaPort, eaJobDescriptor, sizeDesc, tag, dmaTag, isSync, 0, 1); }

static inline int cellSpursJobQueuePortTryPushJob(uint64_t eaPort,
                                                  uint64_t eaJobDescriptor,
                                                  size_t sizeDesc, unsigned tag,
                                                  unsigned int dmaTag,
                                                  unsigned int isSync)
{ return _cellSpursJobQueuePortPushJobBody(eaPort, eaJobDescriptor, sizeDesc, tag, dmaTag, isSync, 0, 0); }

static inline int cellSpursJobQueuePortPushExclusiveJob(uint64_t eaPort,
                                                        uint64_t eaJobDescriptor,
                                                        size_t sizeDesc, unsigned tag,
                                                        unsigned int dmaTag,
                                                        unsigned int isSync)
{ return _cellSpursJobQueuePortPushJobBody(eaPort, eaJobDescriptor, sizeDesc, tag, dmaTag, isSync, 1, 1); }

static inline int cellSpursJobQueuePortTryPushExclusiveJob(uint64_t eaPort,
                                                           uint64_t eaJobDescriptor,
                                                           size_t sizeDesc, unsigned tag,
                                                           unsigned int dmaTag,
                                                           unsigned int isSync)
{ return _cellSpursJobQueuePortPushJobBody(eaPort, eaJobDescriptor, sizeDesc, tag, dmaTag, isSync, 1, 0); }

static inline int cellSpursJobQueuePortCopyPush(uint64_t eaPort,
                                                const CellSpursJobHeader *pJob,
                                                size_t sizeDesc,
                                                unsigned int dmaTag,
                                                unsigned int isSync)
{ return _cellSpursJobQueuePortCopyPushBody(eaPort, pJob, sizeDesc, dmaTag, isSync, 1); }

static inline int cellSpursJobQueuePortCopyPushJob(uint64_t eaPort,
                                                   const CellSpursJobHeader *pJob,
                                                   size_t sizeDesc, unsigned tag,
                                                   unsigned int dmaTag,
                                                   unsigned int isSync)
{ return _cellSpursJobQueuePortCopyPushJobBody(eaPort, pJob, sizeDesc, tag, dmaTag, isSync, 0, 1); }

static inline int cellSpursJobQueuePortTryCopyPushJob(uint64_t eaPort,
                                                      const CellSpursJobHeader *pJob,
                                                      size_t sizeDesc, unsigned tag,
                                                      unsigned int dmaTag,
                                                      unsigned int isSync)
{ return _cellSpursJobQueuePortCopyPushJobBody(eaPort, pJob, sizeDesc, tag, dmaTag, isSync, 0, 0); }

static inline int cellSpursJobQueuePortCopyPushExclusiveJob(uint64_t eaPort,
                                                            const CellSpursJobHeader *pJob,
                                                            size_t sizeDesc, unsigned tag,
                                                            unsigned int dmaTag,
                                                            unsigned int isSync)
{ return _cellSpursJobQueuePortCopyPushJobBody(eaPort, pJob, sizeDesc, tag, dmaTag, isSync, 1, 1); }

static inline int cellSpursJobQueuePortTryCopyPushExclusiveJob(uint64_t eaPort,
                                                               const CellSpursJobHeader *pJob,
                                                               size_t sizeDesc, unsigned tag,
                                                               unsigned int dmaTag,
                                                               unsigned int isSync)
{ return _cellSpursJobQueuePortCopyPushJobBody(eaPort, pJob, sizeDesc, tag, dmaTag, isSync, 1, 0); }

static inline int cellSpursJobQueuePortPushJobList(uint64_t eaPort,
                                                   uint64_t eaJobList,
                                                   unsigned tag,
                                                   unsigned int dmaTag,
                                                   unsigned int isSync)
{ return _cellSpursJobQueuePortPushJobListBody(eaPort, eaJobList, tag, dmaTag, isSync, 1); }

static inline int cellSpursJobQueuePortTryPushJobList(uint64_t eaPort,
                                                      uint64_t eaJobList,
                                                      unsigned tag,
                                                      unsigned int dmaTag,
                                                      unsigned int isSync)
{ return _cellSpursJobQueuePortPushJobListBody(eaPort, eaJobList, tag, dmaTag, isSync, 0); }

static inline int cellSpursJobQueuePortPushFlush(uint64_t eaPort, unsigned int dmaTag)
{ return _cellSpursJobQueuePortPushFlush(eaPort, dmaTag, 1); }

static inline int cellSpursJobQueuePortTryPushFlush(uint64_t eaPort, unsigned int dmaTag)
{ return _cellSpursJobQueuePortPushFlush(eaPort, dmaTag, 0); }

static inline int cellSpursJobQueuePortPushSync(uint64_t eaPort, unsigned tagMask,
                                                unsigned int dmaTag)
{ return _cellSpursJobQueuePortPushSync(eaPort, tagMask, dmaTag, 1); }

static inline int cellSpursJobQueuePortTryPushSync(uint64_t eaPort, unsigned tagMask,
                                                   unsigned int dmaTag)
{ return _cellSpursJobQueuePortPushSync(eaPort, tagMask, dmaTag, 0); }

#endif /* __SPU__ */

#ifdef __cplusplus
} /* extern "C" */
#endif

#include <cell/spurs/job_queue_port_cpp_types.h>

#if defined(__cplusplus) && defined(__SPU__)

__CELL_SPURS_JOBQUEUE_BEGIN

/* SPU handle on a job-queue port in main memory: holds the port's EA
 * and forwards to the EA-based port API.  Not copyable. */
class PortContainer {
protected:
    uint64_t mEaPort;

private:
    PortContainer(const PortContainer &);
    PortContainer &operator=(const PortContainer &);

public:
    PortContainer(uint64_t eaPort) : mEaPort(eaPort) {}
    ~PortContainer() {}

    int initialize(uint64_t eaJobQueue, unsigned isMTSafe = 1)
    { return cellSpursJobQueuePortInitialize(mEaPort, eaJobQueue, isMTSafe); }
    int finalize()
    { return cellSpursJobQueuePortFinalize(mEaPort); }
    uint64_t getJobQueue()
    { return cellSpursJobQueuePortGetJobQueue(mEaPort); }

    int push(uint64_t eaJobDescriptor, size_t sizeDesc,
             unsigned int dmaTag, unsigned int isSync)
    { return cellSpursJobQueuePortPush(mEaPort, eaJobDescriptor, sizeDesc, dmaTag, isSync); }
    int pushJob(uint64_t eaJobDescriptor, size_t sizeDesc, unsigned tag,
                unsigned int dmaTag, unsigned int isSync)
    { return cellSpursJobQueuePortPushJob(mEaPort, eaJobDescriptor, sizeDesc, tag, dmaTag, isSync); }
    int tryPushJob(uint64_t eaJobDescriptor, size_t sizeDesc, unsigned tag,
                   unsigned int dmaTag, unsigned int isSync)
    { return cellSpursJobQueuePortTryPushJob(mEaPort, eaJobDescriptor, sizeDesc, tag, dmaTag, isSync); }
    int pushExclusiveJob(uint64_t eaJobDescriptor, size_t sizeDesc, unsigned tag,
                         unsigned int dmaTag, unsigned int isSync)
    { return cellSpursJobQueuePortPushExclusiveJob(mEaPort, eaJobDescriptor, sizeDesc, tag, dmaTag, isSync); }
    int tryPushExclusiveJob(uint64_t eaJobDescriptor, size_t sizeDesc, unsigned tag,
                            unsigned int dmaTag, unsigned int isSync)
    { return cellSpursJobQueuePortTryPushExclusiveJob(mEaPort, eaJobDescriptor, sizeDesc, tag, dmaTag, isSync); }
    int pushJobList(uint64_t eaJobList, unsigned tag,
                    unsigned int dmaTag, unsigned int isSync)
    { return cellSpursJobQueuePortPushJobList(mEaPort, eaJobList, tag, dmaTag, isSync); }
    int tryPushJobList(uint64_t eaJobList, unsigned tag,
                       unsigned int dmaTag, unsigned int isSync)
    { return cellSpursJobQueuePortTryPushJobList(mEaPort, eaJobList, tag, dmaTag, isSync); }

    int copyPush(const CellSpursJobHeader *pJob, size_t sizeJobDesc,
                 unsigned int dmaTag, unsigned int isSync)
    { return cellSpursJobQueuePortCopyPush(mEaPort, pJob, sizeJobDesc, dmaTag, isSync); }
    int copyPushJob(const CellSpursJobHeader *pJob, size_t sizeJobDesc, unsigned tag,
                    unsigned int dmaTag, unsigned int isSync)
    { return cellSpursJobQueuePortCopyPushJob(mEaPort, pJob, sizeJobDesc, tag, dmaTag, isSync); }
    int tryCopyPushJob(const CellSpursJobHeader *pJob, size_t sizeJobDesc, unsigned tag,
                       unsigned int dmaTag, unsigned int isSync)
    { return cellSpursJobQueuePortTryCopyPushJob(mEaPort, pJob, sizeJobDesc, tag, dmaTag, isSync); }
    int copyPushExclusiveJob(const CellSpursJobHeader *pJob, size_t sizeJobDesc, unsigned tag,
                             unsigned int dmaTag, unsigned int isSync)
    { return cellSpursJobQueuePortCopyPushExclusiveJob(mEaPort, pJob, sizeJobDesc, tag, dmaTag, isSync); }
    int tryCopyPushExclusiveJob(const CellSpursJobHeader *pJob, size_t sizeJobDesc, unsigned tag,
                                unsigned int dmaTag, unsigned int isSync)
    { return cellSpursJobQueuePortTryCopyPushExclusiveJob(mEaPort, pJob, sizeJobDesc, tag, dmaTag, isSync); }

    int sync()
    { return cellSpursJobQueuePortSync(mEaPort); }
    int trySync()
    { return cellSpursJobQueuePortTrySync(mEaPort); }
    int pushFlush(unsigned int dmaTag)
    { return cellSpursJobQueuePortPushFlush(mEaPort, dmaTag); }
    int tryPushFlush(unsigned int dmaTag)
    { return cellSpursJobQueuePortTryPushFlush(mEaPort, dmaTag); }
    int pushSync(unsigned tagMask, unsigned int dmaTag)
    { return cellSpursJobQueuePortPushSync(mEaPort, tagMask, dmaTag); }
    int tryPushSync(unsigned tagMask, unsigned int dmaTag)
    { return cellSpursJobQueuePortTryPushSync(mEaPort, tagMask, dmaTag); }
};

/* A port whose descriptor buffer (numEntries JobType slots) sits in
 * main memory directly after the port object itself. */
template <typename JobType, int numEntries>
class PortWithDescriptorBufferContainer : public PortContainer {
public:
    PortWithDescriptorBufferContainer(uint64_t eaPort) : PortContainer(eaPort) {}
    ~PortWithDescriptorBufferContainer() {}

    int initialize(uint64_t eaJobQueue, unsigned isMTSafe = 1)
    {
        return cellSpursJobQueuePortInitializeWithDescriptorBuffer(
            mEaPort, eaJobQueue, mEaPort + sizeof(CellSpursJobQueuePort),
            sizeof(JobType), numEntries, isMTSafe);
    }
    int copyPush(const JobType *pJob, size_t sizeJobDesc,
                 unsigned int dmaTag, unsigned int isSync)
    { return PortContainer::copyPush(reinterpret_cast<const CellSpursJobHeader *>(pJob), sizeJobDesc, dmaTag, isSync); }
    int copyPushJob(const JobType *pJob, size_t sizeJobDesc, unsigned tag,
                    unsigned int dmaTag, unsigned int isSync)
    { return PortContainer::copyPushJob(reinterpret_cast<const CellSpursJobHeader *>(pJob), sizeJobDesc, tag, dmaTag, isSync); }
    int tryCopyPushJob(const JobType *pJob, size_t sizeJobDesc, unsigned tag,
                       unsigned int dmaTag, unsigned int isSync)
    { return PortContainer::tryCopyPushJob(reinterpret_cast<const CellSpursJobHeader *>(pJob), sizeJobDesc, tag, dmaTag, isSync); }
};

__CELL_SPURS_JOBQUEUE_END

#endif /* __cplusplus && __SPU__ */

#endif /* __PS3DK_CELL_SPURS_JOB_QUEUE_PORT_H__ */
