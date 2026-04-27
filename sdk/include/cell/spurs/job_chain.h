/* cell/spurs/job_chain.h - SPURS job-chain PPU entry-point surface.
 *
 * A job chain is a uint64_t[] (job-command list) running on SPUs as a
 * lightweight workload, terminated by CELL_SPURS_JOB_COMMAND_END.
 * This header exposes:
 *   - CellSpursJobChainAttribute / CellSpursJobChain lifecycle
 *   - Urgent-call injection
 *   - JobGuard initialize / reset / notify
 *   - Exception-handler registration
 *   - Info access (chain + per-stage pipeline snapshots)
 *
 * All entry points resolve to NIDs in libspurs_stub.a.
 */
#ifndef __PS3DK_CELL_SPURS_JOB_CHAIN_H__
#define __PS3DK_CELL_SPURS_JOB_CHAIN_H__

#include <stdint.h>
#include <stdbool.h>
#include <sys/spu_thread.h>
#include <cell/spurs/job_chain_types.h>
#include <cell/spurs/error.h>
#include <cell/spurs/version.h>
#include <cell/spurs/exception_types.h>
#include <cell/spurs/job_descriptor.h>
#include <cell/spurs/job_commands.h>
#include <cell/spurs/job_guard.h>

#ifdef __cplusplus
extern "C" {
#endif

/* -- Attribute ------------------------------------------------------- */

/* Underlying entry point - takes the JM revision + SDK internal version
 * tags so the SPRX can reject ABI-mismatched callers.  Use the inline
 * cellSpursJobChainAttributeInitialize wrapper below in user code. */
extern int _cellSpursJobChainAttributeInitialize(unsigned int jmRevision,
                                                 unsigned int sdkRevision,
                                                 CellSpursJobChainAttribute *attr,
                                                 const uint64_t *jobChainEntry,
                                                 uint16_t sizeJobDescriptor,
                                                 uint16_t maxGrabdedJob,
                                                 const uint8_t priorityTable[8],
                                                 unsigned int maxContention,
                                                 bool autoRequestSpuCount,
                                                 unsigned int tag1,
                                                 unsigned int tag2,
                                                 bool isFixedMemAlloc,
                                                 unsigned int maxSizeJobDescriptor,
                                                 unsigned int initialRequestSpuCount);

extern int cellSpursJobChainAttributeSetName(CellSpursJobChainAttribute *attr,
                                             const char *name);
extern int cellSpursJobChainAttributeSetHaltOnError(CellSpursJobChainAttribute *attr);
extern int cellSpursJobChainAttributeSetJobTypeMemoryCheck(CellSpursJobChainAttribute *attr);

/* -- Lifecycle ------------------------------------------------------- */

extern int cellSpursCreateJobChainWithAttribute(CellSpurs *spurs,
                                                CellSpursJobChain *jobChain,
                                                const CellSpursJobChainAttribute *attr);
extern int cellSpursShutdownJobChain(const CellSpursJobChain *jobChain);
extern int cellSpursRunJobChain(const CellSpursJobChain *jobChain);
extern int cellSpursJoinJobChain(CellSpursJobChain *jobChain);

extern int cellSpursGetJobChainId(const CellSpursJobChain *jobChain,
                                  CellSpursWorkloadId *id);
extern int cellSpursJobChainGetError(CellSpursJobChain *jobChain, void **cause);
extern int cellSpursJobChainGetSpursAddress(const CellSpursJobChain *jobChain,
                                            CellSpurs **ppSpurs);
extern int cellSpursJobSetMaxGrab(CellSpursJobChain *jobChain,
                                  unsigned int maxGrab);

/* -- Urgent calls ---------------------------------------------------- */

extern int cellSpursAddUrgentCall(CellSpursJobChain *jobChain,
                                  uint64_t *commandList);
extern int cellSpursAddUrgentCommand(CellSpursJobChain *jobChain,
                                     uint64_t command);

/* -- Job guard ------------------------------------------------------- */

extern int cellSpursJobGuardInitialize(const CellSpursJobChain *jobChain,
                                       CellSpursJobGuard *jobGuard,
                                       uint32_t notifyCount,
                                       uint8_t requestSpuCount,
                                       uint8_t autoReset);
extern int cellSpursJobGuardReset(CellSpursJobGuard *jobGuard);
extern int cellSpursJobGuardNotify(CellSpursJobGuard *jobGuard);

/* -- Exception event handler ---------------------------------------- */

extern int cellSpursJobChainSetExceptionEventHandler(
    CellSpursJobChain *jobChain,
    CellSpursJobChainExceptionEventHandler handler,
    void *arg);
extern int cellSpursJobChainUnsetExceptionEventHandler(CellSpursJobChain *jobChain);

/* -- Info ------------------------------------------------------------ */

extern int cellSpursGetJobChainInfo(const CellSpursJobChain *jobChain,
                                    CellSpursJobChainInfo *info);
extern int cellSpursGetJobPipelineInfo(CellSpurs *spurs,
                                       sys_spu_thread_t spu_thread,
                                       CellSpursJobPipelineInfo *info);

/* -- Inline wrappers ------------------------------------------------- */

static inline int
cellSpursJobChainAttributeInitialize(CellSpursJobChainAttribute *attr,
                                     const uint64_t *jobChainEntry,
                                     uint16_t sizeJobDescriptor,
                                     uint16_t maxGrabdedJob,
                                     const uint8_t priorityTable[8],
                                     unsigned int maxContention,
                                     bool autoRequestSpuCount,
                                     unsigned int tag1,
                                     unsigned int tag2,
                                     bool isFixedMemAlloc,
                                     unsigned int maxSizeJobDescriptor,
                                     unsigned int initialRequestSpuCount)
{
    return _cellSpursJobChainAttributeInitialize(CELL_SPURS_JOB_REVISION,
                                                 _CELL_SPURS_INTERNAL_VERSION,
                                                 attr,
                                                 jobChainEntry,
                                                 sizeJobDescriptor,
                                                 maxGrabdedJob,
                                                 priorityTable,
                                                 maxContention,
                                                 autoRequestSpuCount,
                                                 tag1,
                                                 tag2,
                                                 isFixedMemAlloc,
                                                 maxSizeJobDescriptor,
                                                 initialRequestSpuCount);
}

#ifdef __cplusplus
}   /* extern "C" */

namespace cell {
namespace Spurs {

class JobChainAttribute : public CellSpursJobChainAttribute {
public:
    static const uint32_t kAlign = CELL_SPURS_JOBCHAIN_ATTRIBUTE_ALIGN;
    static const uint32_t kSize  = CELL_SPURS_JOBCHAIN_ATTRIBUTE_SIZE;

    static int initialize(CellSpursJobChainAttribute *attr,
                          const uint64_t *jobChainEntry,
                          uint16_t sizeJobDescriptor,
                          uint16_t maxGrabdedJob,
                          const uint8_t priorityTable[8],
                          unsigned int maxContention,
                          bool autoRequestSpuCount,
                          unsigned int tag1,
                          unsigned int tag2,
                          bool isFixedMemAlloc,
                          unsigned int maxSizeJobDescriptor,
                          unsigned int initialRequestSpuCount)
    { return cellSpursJobChainAttributeInitialize(attr, jobChainEntry,
                                                  sizeJobDescriptor,
                                                  maxGrabdedJob,
                                                  priorityTable,
                                                  maxContention,
                                                  autoRequestSpuCount,
                                                  tag1, tag2,
                                                  isFixedMemAlloc,
                                                  maxSizeJobDescriptor,
                                                  initialRequestSpuCount); }

    int setName(const char *name)
    { return cellSpursJobChainAttributeSetName(this, name); }

    int setHaltOnError()
    { return cellSpursJobChainAttributeSetHaltOnError(this); }

    int setJobTypeMemoryCheck()
    { return cellSpursJobChainAttributeSetJobTypeMemoryCheck(this); }
};

class JobChain : public CellSpursJobChain {
public:
    static const uint32_t kAlign = CELL_SPURS_JOBCHAIN_ALIGN;
    static const uint32_t kSize  = CELL_SPURS_JOBCHAIN_SIZE;

    static int createWithAttribute(CellSpurs *spurs,
                                   CellSpursJobChain *jobChain,
                                   const CellSpursJobChainAttribute *attr)
    { return cellSpursCreateJobChainWithAttribute(spurs, jobChain, attr); }

    int shutdown() const         { return cellSpursShutdownJobChain(this); }
    int run() const              { return cellSpursRunJobChain(this); }
    int join()                   { return cellSpursJoinJobChain(this); }
    int getId(CellSpursWorkloadId *wid) const
    { return cellSpursGetJobChainId(this, wid); }
    int getError(void **cause)
    { return cellSpursJobChainGetError(this, cause); }
    int getSpursAddress(CellSpurs **ppSpurs) const
    { return cellSpursJobChainGetSpursAddress(this, ppSpurs); }
    int setMaxGrab(unsigned int maxGrab)
    { return cellSpursJobSetMaxGrab(this, maxGrab); }

    int setExceptionEventHandler(CellSpursJobChainExceptionEventHandler &handler,
                                 void *arg)
    { return cellSpursJobChainSetExceptionEventHandler(this, handler, arg); }
    int unsetExceptionEventHandler()
    { return cellSpursJobChainUnsetExceptionEventHandler(this); }

    int getInfo(CellSpursJobChainInfo *info) const
    { return cellSpursGetJobChainInfo(this, info); }
    static int getJobPipelineInfo(CellSpurs *spurs,
                                  sys_spu_thread_t spu_thread,
                                  CellSpursJobPipelineInfo *info)
    { return cellSpursGetJobPipelineInfo(spurs, spu_thread, info); }

    int addUrgentCall(uint64_t *commandList)
    { return cellSpursAddUrgentCall(this, commandList); }
    int addUrgentCommand(uint64_t command)
    { return cellSpursAddUrgentCommand(this, command); }
} __attribute__((aligned(CELL_SPURS_JOBCHAIN_ALIGN)));

class JobGuard : public CellSpursJobGuard {
public:
    static const uint32_t kAlign = CELL_SPURS_JOB_GUARD_ALIGN;
    static const uint32_t kSize  = CELL_SPURS_JOB_GUARD_SIZE;

    static int initialize(const CellSpursJobChain *jobChain,
                          CellSpursJobGuard *jobGuard,
                          uint32_t notifyCount,
                          uint8_t requestSpuCount,
                          uint8_t autoReset)
    { return cellSpursJobGuardInitialize(jobChain, jobGuard,
                                         notifyCount, requestSpuCount,
                                         autoReset); }

    int reset()  { return cellSpursJobGuardReset(this); }
    int notify() { return cellSpursJobGuardNotify(this); }
};

class JobList : public CellSpursJobList {
public:
    static const uint32_t kAlign = CELL_SPURS_JOB_LIST_ALIGN;
    static const uint32_t kSize  = CELL_SPURS_JOB_LIST_SIZE;

    static int initialize(JobList *jl,
                          uint32_t numJobs,
                          uint32_t sizeOfJob,
                          uint64_t eaJobList)
    {
        if ((sizeOfJob & 127) != 0 && sizeOfJob != 64)
            return CELL_SPURS_JOB_ERROR_INVAL;
        if ((eaJobList & 15) != 0)
            return CELL_SPURS_JOB_ERROR_ALIGN;
        jl->numJobs   = numJobs;
        jl->sizeOfJob = sizeOfJob;
        jl->eaJobList = eaJobList;
        return CELL_OK;
    }
};

}   /* namespace Spurs */
}   /* namespace cell */

#endif /* __cplusplus */

#endif /* __PS3DK_CELL_SPURS_JOB_CHAIN_H__ */
