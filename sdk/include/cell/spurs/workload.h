/* cell/spurs/workload.h - SPURS workload C API + C++ wrappers.
 *
 * Clean-room header.  A workload pins a policy-module + user data onto
 * the Spurs SPU scheduler.  Taskset is a specific kind of workload;
 * this header covers the lower-level generic surface used when you
 * ship your own policy module.
 */
#ifndef __PS3DK_CELL_SPURS_WORKLOAD_H__
#define __PS3DK_CELL_SPURS_WORKLOAD_H__

#include <stdint.h>
#include <ppu-types.h>   /* ATTRIBUTE_PRXPTR */
#include <cell/spurs/types.h>

#ifdef __cplusplus
extern "C" {
#endif

#define _CELL_SPURS_WORKLOAD_ATTRIBUTE_REVISION   0x01

#define CELL_SPURS_WORKLOAD_ATTRIBUTE_ALIGN       8
#define CELL_SPURS_WORKLOAD_ATTRIBUTE_SIZE        512

typedef struct CellSpursWorkloadAttribute {
    unsigned char skip[CELL_SPURS_WORKLOAD_ATTRIBUTE_SIZE];
} __attribute__((aligned(CELL_SPURS_WORKLOAD_ATTRIBUTE_ALIGN))) CellSpursWorkloadAttribute;

typedef void (*CellSpursShutdownCompletionEventHook)(CellSpurs *,
                                                     CellSpursWorkloadId,
                                                     void *);

typedef struct CellSpursWorkloadInfo {
    uint64_t                             data;
    uint8_t                              priority[8];
    const void                          *policyModule                         ATTRIBUTE_PRXPTR;
    unsigned int                         sizePolicyModule;
    const char                          *nameClass                            ATTRIBUTE_PRXPTR;
    const char                          *nameInstance                         ATTRIBUTE_PRXPTR;
    uint8_t                              contention;
    uint8_t                              minContention;
    uint8_t                              maxContention;
    uint8_t                              readyCount;
    uint8_t                              idleSpuRequest;
    uint8_t                              hasSignal;
    uint8_t                              padding[2];
    CellSpursShutdownCompletionEventHook shutdownCompletionEventHook          ATTRIBUTE_PRXPTR;
    void                                *shutdownCompletionEventHookArgument  ATTRIBUTE_PRXPTR;
    uint8_t                              reserved[256
                                                  - sizeof(uint64_t)
                                                  - sizeof(uint8_t) * 8
                                                  - 4 /* PRXPTR policyModule */
                                                  - sizeof(unsigned int)
                                                  - 4 * 2 /* PRXPTR nameClass + nameInstance */
                                                  - sizeof(uint8_t) * 8
                                                  - 4 * 2 /* PRXPTR hook + hookArg */];
} CellSpursWorkloadInfo;

/* -- Attribute ------------------------------------------------------- */
extern int _cellSpursWorkloadAttributeInitialize(CellSpursWorkloadAttribute *attr,
                                                 unsigned int revision,
                                                 unsigned int sdkVersion,
                                                 const void *pm,
                                                 unsigned int size,
                                                 uint64_t data,
                                                 const uint8_t priorityTable[8],
                                                 unsigned int minContention,
                                                 unsigned int maxContention);
extern int cellSpursWorkloadAttributeSetName(CellSpursWorkloadAttribute *attr,
                                             const char *nameClass,
                                             const char *nameInstance);
extern int cellSpursWorkloadAttributeSetShutdownCompletionEventHook(
    CellSpursWorkloadAttribute *attr,
    CellSpursShutdownCompletionEventHook hook,
    void *arg);

/* -- Lifecycle ------------------------------------------------------- */
extern int cellSpursAddWorkloadWithAttribute(CellSpurs *spurs,
                                             CellSpursWorkloadId *id,
                                             const CellSpursWorkloadAttribute *attr);
extern int cellSpursAddWorkload(CellSpurs *spurs,
                                CellSpursWorkloadId *id,
                                const void *pm,
                                unsigned int size,
                                uint64_t data,
                                const uint8_t priorityTable[8],
                                unsigned int minContention,
                                unsigned int maxContention);
extern int cellSpursShutdownWorkload(CellSpurs *spurs, CellSpursWorkloadId id);
extern int cellSpursWaitForWorkloadShutdown(CellSpurs *spurs, CellSpursWorkloadId id);
extern int cellSpursRemoveWorkload(CellSpurs *spurs, CellSpursWorkloadId id);

/* -- Signals / data -------------------------------------------------- */
extern int cellSpursSendWorkloadSignal(CellSpurs *spurs, CellSpursWorkloadId id);
extern int cellSpursGetWorkloadData(const CellSpurs *spurs,
                                    uint64_t *data,
                                    CellSpursWorkloadId id);

/* Workload flag: a lightweight one-shot event tied to a workload. */
typedef struct CellSpursWorkloadFlag {
    unsigned char skip[128];
} __attribute__((aligned(128))) CellSpursWorkloadFlag;

extern int cellSpursGetWorkloadFlag(const CellSpurs *spurs,
                                    CellSpursWorkloadFlag **flag);
extern int _cellSpursWorkloadFlagReceiver(CellSpurs *spurs,
                                          CellSpursWorkloadId id,
                                          unsigned int set);
extern int _cellSpursWorkloadFlagReceiver2(CellSpurs *spurs,
                                           CellSpursWorkloadId id,
                                           unsigned int set,
                                           unsigned int isClear);

extern int cellSpursGetWorkloadInfo(const CellSpurs *spurs,
                                    CellSpursWorkloadId id,
                                    CellSpursWorkloadInfo *info);

/* -- Inline wrappers ------------------------------------------------- */
static inline int
cellSpursWorkloadAttributeInitialize(CellSpursWorkloadAttribute *attr,
                                     const void *pm,
                                     unsigned int size,
                                     uint64_t data,
                                     const uint8_t priorityTable[8],
                                     unsigned int minContention,
                                     unsigned int maxContention)
{
    return _cellSpursWorkloadAttributeInitialize(attr,
                                                 _CELL_SPURS_WORKLOAD_ATTRIBUTE_REVISION,
                                                 _CELL_SPURS_INTERNAL_VERSION,
                                                 pm, size, data, priorityTable,
                                                 minContention, maxContention);
}

static inline int
cellSpursSetWorkloadFlagReceiver(CellSpurs *spurs, CellSpursWorkloadId id)
{ return _cellSpursWorkloadFlagReceiver(spurs, id, 1); }

static inline int
cellSpursUnsetWorkloadFlagReceiver(CellSpurs *spurs, CellSpursWorkloadId id)
{ return _cellSpursWorkloadFlagReceiver(spurs, id, 0); }

static inline int
cellSpursSetWorkloadFlagReceiver2(CellSpurs *spurs, CellSpursWorkloadId id)
{ return _cellSpursWorkloadFlagReceiver2(spurs, id, 1, 0); }

static inline int
cellSpursUnsetWorkloadFlagReceiver2(CellSpurs *spurs, CellSpursWorkloadId id)
{ return _cellSpursWorkloadFlagReceiver2(spurs, id, 0, 0); }

#ifdef __cplusplus
}   /* extern "C" */

namespace cell {
namespace Spurs {

struct WorkloadAttribute : public CellSpursWorkloadAttribute {
    static int initialize(CellSpursWorkloadAttribute *attr,
                          const void *pm,
                          unsigned int size,
                          uint64_t data,
                          const uint8_t priorityTable[8],
                          unsigned int minContention,
                          unsigned int maxContention)
    { return cellSpursWorkloadAttributeInitialize(attr, pm, size, data,
                                                  priorityTable,
                                                  minContention, maxContention); }

    int setName(const char *nameClass, const char *nameInstance)
    { return cellSpursWorkloadAttributeSetName(this, nameClass, nameInstance); }

    int setShutdownCompletionEventHook(CellSpursShutdownCompletionEventHook hook,
                                       void *arg)
    { return cellSpursWorkloadAttributeSetShutdownCompletionEventHook(this, hook, arg); }
};

}   /* namespace Spurs */
}   /* namespace cell */

#endif   /* __cplusplus */

#endif   /* __PS3DK_CELL_SPURS_WORKLOAD_H__ */
