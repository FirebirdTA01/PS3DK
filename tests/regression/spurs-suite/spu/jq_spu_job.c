/* spurs-suite SPU job-queue runtime, SPU job (built with
 * -mspurs-job-initialize; the runtime's cellSpursJobMain2 calls this).
 * See jq_spu.h for the modes and the output. */
#include <stdint.h>
#include <spu_intrinsics.h>
#include <spu_mfcio.h>
#include <cell/spurs/job_descriptor.h>
#include <cell/spurs/job_context.h>
#include <cell/spurs/job_queue.h>
#include "../jq_spu.h"

#define JOB_INVAL 0x80410A02u
#define JOB_NULL  0x80410A11u

static uint32_t out[4] __attribute__((aligned(16)));

static void put(uint64_t ea)
{
    mfc_put(out, ea, sizeof out, 0, 0, 0);
    mfc_write_tag_mask(1u << 0);
    mfc_read_tag_status_all();
}

#define EXPECT(step, got, want) do { \
        uint32_t g_ = (uint32_t)(got), w_ = (uint32_t)(want); \
        if (g_ != w_) { out[1] = (step); out[2] = g_; out[3] = w_; return; } } while (0)

static void info(uint64_t jq, uint64_t spurs)
{
    CellSpursJobQueueHandle h = -1;
    CellSpursWorkloadId wid = 99;
    int exitCode = -1;
    void *cause = (void *)1;
    int n;
    EXPECT(1, cellSpursJobQueueOpen(jq, &h), 0);
    n = cellSpursJobQueueGetHandleCount(jq);
    EXPECT(2, n >= 2, 1);                               /* the PPU's and ours */
    EXPECT(3, (uint32_t)cellSpursJobQueueGetSpurs(jq), (uint32_t)spurs);
    EXPECT(4, cellSpursJobQueueGetMaxSizeJobDescriptor(jq), Q_MAX_DESC);
    EXPECT(5, cellSpursGetJobQueueId(jq, &wid), 0);
    EXPECT(6, wid < 32, 1);
    EXPECT(7, cellSpursJobQueueGetError(jq, &exitCode, &cause), 0);
    EXPECT(8, exitCode, 0);
    EXPECT(9, (uintptr_t)cause, 0);
    EXPECT(10, cellSpursJobQueueClose(jq, h), 0);
    EXPECT(11, cellSpursJobQueueGetHandleCount(jq), n - 1);
    EXPECT(12, cellSpursJobQueueClose(jq, h), JOB_INVAL);       /* no longer open */
    EXPECT(13, cellSpursJobQueueClose(jq, 2000), JOB_INVAL);
    EXPECT(14, cellSpursJobQueueOpen(0, &h), JOB_NULL);
    EXPECT(15, cellSpursJobQueueGetError(jq, 0, &cause), JOB_NULL);
}

/* push two jobs with a semaphore through our own handle (try forms: a
   job may not block), then flush and a sync */
static void push(CellSpursJob256 *job)
{
    const uint64_t jq = job->workArea.userData[2];
    CellSpursJobQueueHandle h = -1;
    EXPECT(20, cellSpursJobQueueOpen(jq, &h), 0);
    EXPECT(21, _cellSpursJobQueuePushJobBody(jq, h, job->workArea.userData[4], 128, 0, 2,
                                             job->workArea.userData[3], 0, 0), 0);
    EXPECT(22, _cellSpursJobQueuePushJob2Body(jq, h, job->workArea.userData[5], 128, 0, 2,
                                              4 /* do not block */, job->workArea.userData[3]), 0);
    EXPECT(29, _cellSpursJobQueuePushJob2Body(jq, h, job->workArea.userData[5], 128, 0, 2, 8, 0), JOB_INVAL);
    EXPECT(23, _cellSpursJobQueuePushJobBody(jq, 1000, job->workArea.userData[4], 128, 0, 2, 0, 0, 0), JOB_INVAL);
    EXPECT(24, _cellSpursJobQueuePushJobBody(jq, h, job->workArea.userData[4], 96, 0, 2, 0, 0, 0), JOB_INVAL);
    EXPECT(25, _cellSpursJobQueuePushFlush(jq, h, 2, 0), 0);
    EXPECT(26, _cellSpursJobQueuePushSync(jq, h, 1, 2, 0), 0);
    EXPECT(27, _cellSpursJobQueuePushSync(jq, h, 0, 2, 0), JOB_INVAL);
    EXPECT(28, cellSpursJobQueueClose(jq, h), 0);
}

void cellSpursJobQueueMain(CellSpursJobContext2 *ctx, CellSpursJob256 *job)
{
    const uint64_t dst = job->workArea.userData[0];
    const unsigned mode = (unsigned)job->workArea.userData[1];
    (void)ctx;
    out[0] = out[1] = out[2] = out[3] = 0;
    if (mode == Q_INFO) {
        info(job->workArea.userData[2], job->workArea.userData[3]);
    } else if (mode == Q_PUSH) {
        push(job);
    } else if (mode == Q_WAIT) {
        out[1] = 1;                                     /* about to suspend */
        put(dst);
        out[1] = (uint32_t)cellSpursJobQueueWaitSignal(job->workArea.userData[3]);
    }
    out[0] = Q_MAGIC + mode;
    put(dst);
}
