/* spurs-suite SPU job-queue runtime, SPU job (built with
 * -mspurs-job-initialize; the runtime's cellSpursJobMain2 calls this).
 * See jq_spu.h for the modes and the output. */
#include <stdint.h>
#include <spu_intrinsics.h>
#include <spu_mfcio.h>
#include <cell/spurs/job_descriptor.h>
#include <cell/spurs/job_context.h>
#include <cell/spurs/job_queue.h>
#include <cell/spurs/job_queue_port.h>
#include <cell/spurs/job_queue_port2.h>
#include "../jq_spu.h"

#define JOB_AGAIN 0x80410A01u
#define JOB_INVAL 0x80410A02u
#define JOB_BUSY  0x80410A0Au
#define JOB_STAT  0x80410A0Fu
#define JOB_ALIGN 0x80410A10u
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

/* ports: the PPU syncs and finalizes them afterwards */
static uint8_t desc[128] __attribute__((aligned(128)));

static void port(CellSpursJob256 *job)
{
    const uint64_t jq = job->workArea.userData[2], p1 = job->workArea.userData[3], p2 = job->workArea.userData[5];
    EXPECT(40, cellSpursJobQueuePortInitialize(p1, jq, 0), 0);
    EXPECT(41, cellSpursJobQueuePortGetJobQueue(p1), jq);
    EXPECT(42, _cellSpursJobQueuePortPushJobBody(p1, job->workArea.userData[4], 128, 0, 2, 1, 0, 0), 0);
    EXPECT(43, _cellSpursJobQueuePortPushFlush(p1, 2, 0), 0);
    EXPECT(44, cellSpursJobQueuePortInitializeWithDescriptorBuffer(p2, jq, job->workArea.userData[7], 128, 1, 0), 0);
    mfc_get(desc, job->workArea.userData[6], sizeof desc, 2, 0, 0);
    mfc_write_tag_mask(1u << 2);
    mfc_read_tag_status_all();
    EXPECT(45, _cellSpursJobQueuePortCopyPushJobBody(p2, desc, 128, 0, 2, 1, 0, 0), 0);
    EXPECT(46, _cellSpursJobQueuePortCopyPushJobBody(p2, desc, 128, 0, 2, 1, 0, 0), JOB_AGAIN);   /* one entry */
    EXPECT(47, _cellSpursJobQueuePortPushFlush(p2, 2, 0), 0);
}

/* Port2 made here; the PPU syncs and destroys it */
static void port2(CellSpursJob256 *job)
{
    const uint64_t jq = job->workArea.userData[2], p = job->workArea.userData[3];
    EXPECT(60, cellSpursJobQueuePort2Create(0, jq), JOB_NULL);
    EXPECT(61, cellSpursJobQueuePort2Create(p + 16, jq), JOB_ALIGN);
    EXPECT(62, cellSpursJobQueuePort2Create(p, jq), 0);
    EXPECT(63, cellSpursJobQueuePort2GetJobQueue(p), jq);
    EXPECT(64, cellSpursJobQueuePort2PushJob(p, job->workArea.userData[4], 128, 0, 2, 8), JOB_INVAL);
    EXPECT(65, cellSpursJobQueuePort2PushJob(p, job->workArea.userData[4], 128, 0, 2, 1 | 4), 0);
    EXPECT(66, cellSpursJobQueuePort2PushJobList(p, job->workArea.userData[5], 0, 2, 2), JOB_INVAL);
    EXPECT(67, cellSpursJobQueuePort2PushJobList(p, job->workArea.userData[5], 0, 2, 1 | 4), 0);
    EXPECT(68, cellSpursJobQueuePort2PushSync(p, 1, 2, 4), 0);
    EXPECT(69, cellSpursJobQueuePort2PushFlush(p, 2, 4), 0);
    EXPECT(70, cellSpursJobQueuePort2PushFlush(p, 2, 1), JOB_INVAL);
    EXPECT(71, cellSpursJobQueuePort2Sync(p, 8), JOB_INVAL);
    EXPECT(72, cellSpursJobQueuePort2Destroy(p), JOB_BUSY);          /* two sync jobs not waited for */
}

/* Port2 made by the PPU: nothing pending, so a try-sync succeeds */
static void port2_sync(CellSpursJob256 *job)
{
    const uint64_t p = job->workArea.userData[3];
    EXPECT(80, cellSpursJobQueuePort2GetJobQueue(p), job->workArea.userData[2]);
    EXPECT(81, cellSpursJobQueuePort2Sync(p, 4), 0);
    EXPECT(82, cellSpursJobQueuePort2Destroy(p), 0);
    EXPECT(83, cellSpursJobQueuePort2Destroy(p), JOB_STAT);
}

/* suspended-job sizes, compared with the PPU's by the row */
static void susp_size(CellSpursJob256 *job)
{
    static uint8_t desc[256] __attribute__((aligned(128)));
    static uint32_t res[4] __attribute__((aligned(16)));
    static uint32_t sizes[(Q_SS_CASES + 3) & ~3] __attribute__((aligned(16)));
    unsigned i, attr;
    mfc_get(sizes, job->workArea.userData[6], sizeof sizes, 3, 0, 0);
    mfc_write_tag_mask(1u << 3);
    mfc_read_tag_status_all();
    for (i = 0; i < Q_SS_CASES; ++i) {
        mfc_get(desc, job->workArea.userData[4] + 256 * i, 256, 3, 0, 0);
        mfc_read_tag_status_all();
        for (attr = 0; attr < 2; ++attr) {
            unsigned size = 0xdeadu;
            res[2 * attr] = (uint32_t)cellSpursJobQueueGetSuspendedJobSize((const CellSpursJobHeader *)desc, sizes[i],
                                                                          (enum CellSpursJobQueueSuspendedJobAttribute)attr,
                                                                          &size);
            res[2 * attr + 1] = size;
        }
        mfc_put(res, job->workArea.userData[5] + 16 * i, 16, 3, 0, 0);
        mfc_read_tag_status_all();
    }
}

void cellSpursJobQueueMain(CellSpursJobContext2 *ctx, CellSpursJob256 *job)
{
    const uint64_t dst = job->workArea.userData[0];
    const unsigned mode = (unsigned)job->workArea.userData[1];
    (void)ctx;
    out[0] = out[1] = out[2] = out[3] = 0;
    if (mode == Q_INFO) {
        info(job->workArea.userData[2], job->workArea.userData[3]);
    } else if (mode == Q_PORT) {
        port(job);
    } else if (mode == Q_PORT2) {
        port2(job);
    } else if (mode == Q_PORT2S) {
        port2_sync(job);
    } else if (mode == Q_PUSH) {
        push(job);
    } else if (mode == Q_SUSPSIZE) {
        susp_size(job);
    } else if (mode == Q_SIGNAL) {
        const uint64_t susp = job->workArea.userData[3];
        if (cellSpursJobQueueSendSignal(0) != (int)JOB_NULL) {
            out[1] = 90;
        } else if (cellSpursJobQueueSendSignal(susp + 16) != (int)JOB_ALIGN) {
            out[1] = 91;
        } else {
            out[2] = (uint32_t)cellSpursJobQueueSendSignal(susp);
            if (out[2])
                out[1] = 92;
        }
    } else if (mode == Q_SLOW) {
        const uint32_t start = spu_readch(SPU_RdDec);
        while (start - spu_readch(SPU_RdDec) < 200000)
            ;
    } else if (mode == Q_WAIT) {
        out[1] = 1;                                     /* about to suspend */
        put(dst);
        out[1] = (uint32_t)cellSpursJobQueueWaitSignal(job->workArea.userData[3]);
    }
    out[0] = Q_MAGIC + mode;
    put(dst);
}
