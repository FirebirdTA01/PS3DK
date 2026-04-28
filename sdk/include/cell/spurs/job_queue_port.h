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

#endif /* __SPU__ */

#ifdef __cplusplus
} /* extern "C" */
#endif

#include <cell/spurs/job_queue_port_cpp_types.h>

#endif /* __PS3DK_CELL_SPURS_JOB_QUEUE_PORT_H__ */
