#ifndef __PS3DK_SCE_HL_SPURS_COMMAND_LIST_DISPATCHER_H__
#define __PS3DK_SCE_HL_SPURS_COMMAND_LIST_DISPATCHER_H__

#include <cell/spurs.h>
#include <sce/hl/spurs/event_flag.h>
#include <sce/hl/spurs/job.h>
#include <sce/hl/spurs/job_chain.h>
#include <sce/hl/spurs/job_guard.h>

#ifdef __cplusplus
__SCE_HL_SPURS_BEGIN

class CommandListDispatcher : public JobChain {
    CellSpursJobList mJobList;
    uint32_t mHasSet;
    uint32_t __reserved__;
    Job::Command mCommand[11];
    EventFlag mEventFlag;
    JobGuard mGuard;
    Job::Notification mNotification;
    uint64_t __aligned_to_128bytes__[8];

public:
    static int create(CommandListDispatcher *jobChain,
                      const char *name,
                      Spurs *spurs,
                      uint16_t sizeJobDescriptor,
                      uint8_t priority = CELL_SPURS_MAX_PRIORITY - 1,
                      uint8_t spuMask = 0xff)
    {
        jobChain->mHasSet = 0;
        __SCE_SPURS_UTIL_RETURN_IF(EventFlag::initialize(&jobChain->mEventFlag, spurs));
        __SCE_SPURS_UTIL_RETURN_IF(Job::Notification::initialize(
            &jobChain->mNotification, &jobChain->mEventFlag));
        jobChain->mJobList.numJobs = 1;
        jobChain->mJobList.sizeOfJob = sizeof(Job::Notification);
        jobChain->mJobList.eaJobList = reinterpret_cast<uintptr_t>(&jobChain->mNotification);
        jobChain->mCommand[0] = Job::Guard(jobChain->mGuard);
        jobChain->mCommand[1] = Job::End();
        jobChain->mCommand[2] = Job::Sync();
        jobChain->mCommand[3] = Job::JobList(&jobChain->mJobList);
        jobChain->mCommand[4] = Job::Sync();
        jobChain->mCommand[5] = Job::Flush();
        jobChain->mCommand[6] = Job::ResetPC(jobChain->mCommand);
        __SCE_SPURS_UTIL_RETURN_IF(JobChain::create(
            jobChain, name, spurs, jobChain->mCommand,
            sizeJobDescriptor, priority, spuMask));
        return JobGuard::initialize(&jobChain->mGuard, jobChain, 1, 8, 1);
    }

    int dispatch(const Job::Command *command)
    {
        if (mHasSet) return CELL_SPURS_JOB_ERROR_BUSY;
        mHasSet = 1;
        mCommand[1] = Job::Call(command);
        __SCE_SPURS_UTIL_RETURN_IF(mGuard.notify());
        return run();
    }

    int wait()
    {
        if (!mHasSet) return CELL_OK;
        __SCE_SPURS_UTIL_RETURN_IF(mNotification.wait());
        mHasSet = 0;
        return CELL_OK;
    }
} __attribute__((aligned(CELL_SPURS_JOBCHAIN_ALIGN)));

__SCE_HL_SPURS_END
#endif /* __cplusplus */

#endif /* __PS3DK_SCE_HL_SPURS_COMMAND_LIST_DISPATCHER_H__ */
