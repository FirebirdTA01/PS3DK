/*
 * hello-spurs-jq — cell/spurs/job_queue*.h header validation probe.
 *
 * Compile-and-link test that touches every entry point declared
 * across the four cell/spurs/job_queue*.h PPU headers + their cpp
 * wrapper classes.  Each call site is gated by `if (false)` so the
 * sample doesn't actually try to drive the queue at runtime — the
 * SPU runtime (libspurs_jq.a) isn't ported yet.  Purpose: surface
 * header / struct ABI errors before the runtime work.
 */

#include <cstdio>
#include <cstring>

#include <sys/process.h>
#include <cell/spurs.h>
#include <cell/spurs/job_queue.h>
#include <cell/spurs/job_queue_port.h>
#include <cell/spurs/job_queue_port2.h>
#include <cell/spurs/job_queue_semaphore.h>

SYS_PROCESS_PARAM(1001, 0x10000);

static CellSpursJobQueue          s_jq          __attribute__((aligned(128)));
static CellSpursJobQueueAttribute s_jqAttr      __attribute__((aligned(8)));
static CellSpursJobQueuePort      s_port        __attribute__((aligned(128)));
static CellSpursJobQueuePort2     s_port2       __attribute__((aligned(128)));
static CellSpursJobQueueSemaphore s_sem         __attribute__((aligned(128)));
static CellSpursJob256            s_job         __attribute__((aligned(128)));
static uint64_t                   s_chain[16]   __attribute__((aligned(16)));
static const uint8_t              s_priority[8] = {8, 0, 0, 0, 0, 0, 0, 0};

/* Force the linker to keep every entry point we can call from headers
 * - if any signature is wrong the link fails with an "undefined NID
 * reference" or a duplicate-definition error. */
static int touch_ppu_api(cell::Spurs::Spurs *spurs)
{
    int rc = 0;

    /* attribute setters (every variant) */
    rc |= cellSpursJobQueueAttributeInitialize(&s_jqAttr);
    rc |= cellSpursJobQueueAttributeSetMaxGrab(&s_jqAttr, 4);
    rc |= cellSpursJobQueueAttributeSetSubmitWithEntryLock(&s_jqAttr, false);
    rc |= cellSpursJobQueueAttributeSetDoBusyWaiting(&s_jqAttr, false);
    rc |= cellSpursJobQueueAttributeSetIsHaltOnError(&s_jqAttr, true);
    rc |= cellSpursJobQueueAttributeSetIsJobTypeMemoryCheck(&s_jqAttr, false);
    rc |= cellSpursJobQueueAttributeSetMaxSizeJobDescriptor(&s_jqAttr, 256);
    rc |= cellSpursJobQueueAttributeSetGrabParameters(&s_jqAttr, 16, 4);

    /* descriptor pool size helper */
    CellSpursJobQueueJobDescriptorPool pool = {};
    pool.nJob256 = 8;
    int poolBytes = cellSpursJobQueueGetJobDescriptorPoolSize(&pool);
    rc |= (poolBytes < 0 ? poolBytes : 0);

    /* lifecycle (inline wrappers + underlying NIDs) */
    rc |= cellSpursCreateJobQueue((CellSpurs *)spurs, &s_jq, &s_jqAttr,
                                  "hello-jq", s_chain, 8, 1, s_priority);
    rc |= cellSpursShutdownJobQueue(&s_jq);
    int exitCode = 0;
    rc |= cellSpursJoinJobQueue(&s_jq, &exitCode);

    /* handle open/close */
    CellSpursJobQueueHandle h = CELL_SPURS_JOBQUEUE_HANDLE_INVALID;
    rc |= cellSpursJobQueueOpen(&s_jq, &h);
    rc |= cellSpursJobQueueClose(&s_jq, h);

    /* info */
    CellSpurs *spursPtr = cellSpursJobQueueGetSpurs(&s_jq); (void)spursPtr;
    rc |= cellSpursJobQueueGetHandleCount(&s_jq);
    void *cause = nullptr;
    rc |= cellSpursJobQueueGetError(&s_jq, &exitCode, &cause);
    rc |= cellSpursJobQueueGetMaxSizeJobDescriptor(&s_jq);
    CellSpursWorkloadId wid = 0;
    rc |= cellSpursGetJobQueueId(&s_jq, &wid);
    unsigned int suspSize = 0;
    rc |= cellSpursJobQueueGetSuspendedJobSize(
        &s_job.header, sizeof(s_job),
        CELL_SPURS_JOBQUEUE_JOB_SAVE_ALL, &suspSize);

    /* exception handler */
    rc |= cellSpursJobQueueSetExceptionEventHandler(&s_jq, nullptr, nullptr);
    rc |= cellSpursJobQueueSetExceptionEventHandler2(&s_jq, nullptr, nullptr);
    rc |= cellSpursJobQueueUnsetExceptionEventHandler(&s_jq);

    /* state */
    rc |= cellSpursJobQueueSetWaitingMode(&s_jq, CELL_SPURS_JOBQUEUE_WAITING_MODE_SLEEP);

    /* push variants (handle-side direct push) */
    CellSpursJobHeader *allocated = nullptr;
    rc |= cellSpursJobQueueAllocateJobDescriptor(&s_jq, h, sizeof(s_job), 0, &allocated);
    rc |= cellSpursJobQueuePushAndReleaseJob(&s_jq, h, &s_job.header, sizeof(s_job),
                                             0, 0, &s_sem);
    rc |= cellSpursJobQueuePushJob2(&s_jq, h, &s_job.header, sizeof(s_job),
                                    0, 0, &s_sem);
    rc |= cellSpursJobQueuePush(&s_jq, h, &s_job.header, sizeof(s_job), &s_sem);
    rc |= cellSpursJobQueuePushJob(&s_jq, h, &s_job.header, sizeof(s_job), 0, &s_sem);
    rc |= cellSpursJobQueuePushExclusiveJob(&s_jq, h, &s_job.header, sizeof(s_job), 0, &s_sem);
    rc |= cellSpursJobQueueTryPush(&s_jq, h, &s_job.header, sizeof(s_job), &s_sem);
    rc |= cellSpursJobQueueTryPushJob(&s_jq, h, &s_job.header, sizeof(s_job), 0, &s_sem);
    rc |= cellSpursJobQueueTryPushExclusiveJob(&s_jq, h, &s_job.header, sizeof(s_job), 0, &s_sem);
    rc |= cellSpursJobQueuePushFlush(&s_jq, h);
    rc |= cellSpursJobQueueTryPushFlush(&s_jq, h);
    rc |= cellSpursJobQueuePushSync(&s_jq, h, 0xffffffffu);
    rc |= cellSpursJobQueueTryPushSync(&s_jq, h, 0xffffffffu);
    rc |= cellSpursJobQueueSendSignal((CellSpursJobQueueWaitingJob *)&s_job);

    /* port (port_types + port.h) */
    rc |= cellSpursJobQueuePortInitialize(&s_port, &s_jq, true);
    rc |= cellSpursJobQueuePortInitializeWithDescriptorBuffer(
        &s_port, &s_jq, &s_job.header, 256, 4, true);
    rc |= cellSpursJobQueuePortFinalize(&s_port);
    CellSpursJobQueue *portQ = cellSpursJobQueuePortGetJobQueue(&s_port); (void)portQ;
    rc |= cellSpursJobQueuePortPush(&s_port, &s_job.header, sizeof(s_job), false);
    rc |= cellSpursJobQueuePortCopyPush(&s_port, &s_job.header, sizeof(s_job), false);
    rc |= cellSpursJobQueuePortPushJob(&s_port, &s_job.header, sizeof(s_job), 0, false);
    rc |= cellSpursJobQueuePortPushExclusiveJob(&s_port, &s_job.header, sizeof(s_job), 0, false);
    rc |= cellSpursJobQueuePortCopyPushJob(&s_port, &s_job.header, sizeof(s_job), 0, false);
    rc |= cellSpursJobQueuePortCopyPushExclusiveJob(&s_port, &s_job.header, sizeof(s_job), 0, false);
    rc |= cellSpursJobQueuePortTryPush(&s_port, &s_job.header, sizeof(s_job), false);
    rc |= cellSpursJobQueuePortTryCopyPush(&s_port, &s_job.header, sizeof(s_job), false);
    rc |= cellSpursJobQueuePortTryPushJob(&s_port, &s_job.header, sizeof(s_job), 0, false);
    rc |= cellSpursJobQueuePortTryPushExclusiveJob(&s_port, &s_job.header, sizeof(s_job), 0, false);
    rc |= cellSpursJobQueuePortTryCopyPushJob(&s_port, &s_job.header, sizeof(s_job), 0, false);
    rc |= cellSpursJobQueuePortTryCopyPushExclusiveJob(&s_port, &s_job.header, sizeof(s_job), 0, false);
    rc |= cellSpursJobQueuePortSync(&s_port);
    rc |= cellSpursJobQueuePortTrySync(&s_port);
    rc |= cellSpursJobQueuePortPushFlush(&s_port);
    rc |= cellSpursJobQueuePortTryPushFlush(&s_port);
    rc |= cellSpursJobQueuePortPushSync(&s_port, 0xffffffffu);
    rc |= cellSpursJobQueuePortTryPushSync(&s_port, 0xffffffffu);

    /* port2 (port2_types + port2.h) */
    rc |= cellSpursJobQueuePort2Create(&s_port2, &s_jq);
    rc |= cellSpursJobQueuePort2Destroy(&s_port2);
    CellSpursJobQueue *port2Q = cellSpursJobQueuePort2GetJobQueue(&s_port2); (void)port2Q;
    rc |= cellSpursJobQueuePort2PushJob(&s_port2, &s_job.header, sizeof(s_job), 0, 0);
    rc |= cellSpursJobQueuePort2CopyPushJob(&s_port2, &s_job.header, sizeof(s_job), 256, 0, 0);
    rc |= cellSpursJobQueuePort2PushAndReleaseJob(&s_port2, &s_job.header, sizeof(s_job), 0, 0);
    CellSpursJobHeader *p2alloc = nullptr;
    rc |= cellSpursJobQueuePort2AllocateJobDescriptor(&s_port2, sizeof(s_job), 0, &p2alloc);
    rc |= cellSpursJobQueuePort2Sync(&s_port2, 0);
    rc |= cellSpursJobQueuePort2PushFlush(&s_port2, 0);
    rc |= cellSpursJobQueuePort2PushSync(&s_port2, 0xffffffffu, 0);

    /* semaphore */
    rc |= cellSpursJobQueueSemaphoreInitialize(&s_sem, &s_jq);
    rc |= cellSpursJobQueueSemaphoreAcquire(&s_sem, 1);
    rc |= cellSpursJobQueueSemaphoreTryAcquire(&s_sem, 1);

    /* C++ wrapper class instantiations (also exercise the inline
     * forwarders + size constants) */
    using cell::Spurs::JobQueue::JobQueueBase;
    using cell::Spurs::JobQueue::Port;
    using cell::Spurs::JobQueue::Port2;
    using cell::Spurs::JobQueue::PortWithDescriptorBuffer;

    static_assert(JobQueueBase::kAlign == CELL_SPURS_JOBQUEUE_ALIGN, "");
    static_assert(JobQueueBase::kSize  == CELL_SPURS_JOBQUEUE_SIZE,  "");
    static_assert(Port::kAlign  == CELL_SPURS_JOBQUEUE_PORT_ALIGN,  "");
    static_assert(Port::kSize   == CELL_SPURS_JOBQUEUE_PORT_SIZE,   "");
    static_assert(Port2::kAlign == CELL_SPURS_JOBQUEUE_PORT2_ALIGN, "");
    static_assert(Port2::kSize  == CELL_SPURS_JOBQUEUE_PORT2_SIZE,  "");

    /* PortWithDescriptorBuffer<JobT, N> instantiation - just compile-test. */
    using PortBuf = PortWithDescriptorBuffer<CellSpursJob256, 4>;
    static PortBuf s_portBuf __attribute__((aligned(128)));
    rc |= s_portBuf.initialize(&s_jq, true);

    return rc;
}

int main(void)
{
    std::printf("hello-spurs-jq: header validation probe (link-only)\n");

    /* Skip runtime exercise — SPU runtime not yet ported.  We just
     * need the link to succeed. */
    if (false) {
        cell::Spurs::Spurs *spurs = new cell::Spurs::Spurs;
        int rc = touch_ppu_api(spurs);
        delete spurs;
        return rc;
    }

    std::printf("OK (compile + link succeeded; runtime wired, awaiting SPU lib)\n");
    return 0;
}
