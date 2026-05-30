#ifndef __PS3DK_SCE_HL_SPURS_JOB_CHAIN_H__
#define __PS3DK_SCE_HL_SPURS_JOB_CHAIN_H__

#include <stdint.h>
#include <cell/spurs.h>
#include <sce/hl/spurs/define.h>
#include <sce/hl/spurs/job.h>
#include <sce/hl/spurs/spurs.h>

#ifdef __cplusplus
__SCE_HL_SPURS_BEGIN

class JobChain : public cell::Spurs::JobChain {
public:
    static const uint32_t ALIGN = CELL_SPURS_JOBCHAIN_ALIGN;

    JobChain() {}

    static int create(JobChain *jobChain,
                      const char *name,
                      Spurs *spurs,
                      const uint64_t *jobChainEntry,
                      uint16_t sizeJobDescriptor,
                      uint8_t priority = CELL_SPURS_MAX_PRIORITY - 1,
                      uint8_t spuMask = 0xff)
    {
        uint8_t priorities[CELL_SPURS_MAX_SPU];
        for (unsigned i = 0; i < CELL_SPURS_MAX_SPU; ++i) {
            priorities[i] = (spuMask & (1u << i)) ? priority : 0;
        }

        CellSpursJobChainAttribute attr;
        __SCE_SPURS_UTIL_RETURN_IF(cellSpursJobChainAttributeInitialize(
            &attr, jobChainEntry, sizeJobDescriptor, 4, priorities, 8,
            true, 0, 1, false, 1024, 8));
        if (name) {
            __SCE_SPURS_UTIL_RETURN_IF(cellSpursJobChainAttributeSetName(&attr, name));
        }
        __SCE_SPURS_UTIL_RETURN_IF(cellSpursJobChainAttributeSetHaltOnError(&attr));
        return create(jobChain, spurs, &attr);
    }

    static int create(JobChain *jobChain,
                      const char *name,
                      Spurs *spurs,
                      const Job::Command *commandList,
                      uint16_t sizeJobDescriptor,
                      uint8_t priority = CELL_SPURS_MAX_PRIORITY - 1,
                      uint8_t spuMask = 0xff)
    {
        return create(jobChain, name, spurs,
                      reinterpret_cast<const uint64_t *>(commandList),
                      sizeJobDescriptor, priority, spuMask);
    }

    static int create(JobChain *jobChain,
                      Spurs *spurs,
                      CellSpursJobChainAttribute *attr)
    { return cell::Spurs::JobChain::createWithAttribute(spurs, jobChain, attr); }
} __attribute__((aligned(CELL_SPURS_JOBCHAIN_ALIGN)));

__SCE_HL_SPURS_END
#endif /* __cplusplus */

#endif /* __PS3DK_SCE_HL_SPURS_JOB_CHAIN_H__ */
