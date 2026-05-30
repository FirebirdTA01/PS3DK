#ifndef __PS3DK_SCE_HL_SPURS_JOB_GUARD_H__
#define __PS3DK_SCE_HL_SPURS_JOB_GUARD_H__

#include <stdint.h>
#include <cell/spurs.h>
#include <sce/hl/spurs/define.h>
#include <sce/hl/spurs/job_chain.h>

#ifdef __cplusplus
__SCE_HL_SPURS_BEGIN

class JobGuard : public cell::Spurs::JobGuard {
public:
    static const uint32_t ALIGN = CELL_SPURS_JOB_GUARD_ALIGN;

    JobGuard() {}

    static int initialize(JobGuard *guard,
                          const JobChain *jobChain,
                          uint32_t notifyCount,
                          uint8_t numReadyCount = 8,
                          uint8_t autoReset = 1)
    {
        return cell::Spurs::JobGuard::initialize(
            jobChain, guard, notifyCount, numReadyCount, autoReset);
    }
} __attribute__((aligned(CELL_SPURS_JOB_GUARD_ALIGN)));

__SCE_HL_SPURS_END
#endif /* __cplusplus */

#endif /* __PS3DK_SCE_HL_SPURS_JOB_GUARD_H__ */
