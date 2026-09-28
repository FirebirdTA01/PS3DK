/* spurs-suite job queue (row spurs-job-queue): six jobs built with the
 * -mspurs-job-initialize driver mode are pushed through a job-queue handle
 * with a job-queue semaphore; the PPU acquires the semaphore for all six,
 * then checks every result slot and tears the queue down. */
#include "harness.h"
#include <cell/spurs/job_queue.h>
#include <cell/spurs/job_queue_semaphore.h>
#include <cell/sysmodule.h>
#include SUITE_SPU_HEADER
#include SUITE_SPU_JOBHEADER_HEADER

static const unsigned kJobs = 6;
static const uint32_t kMagic = 0xc0ffe100u;
#define JQ_DEPTH 16
#define JQ_POOL (kJobs + 2)

static CellSpursJobQueue s_jq __attribute__((aligned(128)));
static CellSpursJobQueueSemaphore s_sem __attribute__((aligned(128)));
static CellSpursJob128 s_job[kJobs] __attribute__((aligned(128)));
static uint64_t s_cmd[CELL_SPURS_JOBQUEUE_SIZE_COMMAND_BUFFER(JQ_DEPTH) / sizeof(uint64_t)]
    __attribute__((aligned(CELL_SPURS_JOBQUEUE_COMMAND_BUFFER_ALIGN)));
static volatile uint32_t s_out[kJobs][4] __attribute__((aligned(128)));
static const uint8_t s_prio[8] = { 8, 0, 0, 0, 0, 0, 0, 0 };
static uint8_t s_pool[CELL_SPURS_JOBQUEUE_JOB_DESCRIPTOR_POOL_SIZE(0, JQ_POOL, 0, 0, 0, 0, 0, 0)]
    __attribute__((aligned(CELL_SPURS_JOBQUEUE_JOB_DESCRIPTOR_POOL_ALIGN)));

static int row_main()
{
    int rc = cellSysmoduleLoadModule(CELL_SYSMODULE_SPURS_JQ);
    if (rc) return suite::invalid("load SPURS_JQ", rc);
    auto *spurs = new cell::Spurs::Spurs2;
    if ((rc = suite::spurs_up(spurs, "SuiteJq"))) return suite::invalid("spurs", rc);

    CellSpursJobQueueAttribute attr;
    if ((rc = cellSpursJobQueueAttributeInitialize(&attr))) return suite::invalid("jq attribute", rc);
    cellSpursJobQueueAttributeSetMaxSizeJobDescriptor(&attr, 256);
    cellSpursJobQueueAttributeSetIsHaltOnError(&attr, true);
    CellSpursJobQueueJobDescriptorPool poolDesc;
    std::memset(&poolDesc, 0, sizeof poolDesc);
    poolDesc.nJob128 = JQ_POOL;
    rc = cellSpursCreateJobQueueWithJobDescriptorPool(reinterpret_cast<CellSpurs *>(spurs), &s_jq, &attr, s_pool, &poolDesc, "suite-jq",
                                                      s_cmd, JQ_DEPTH, 2, s_prio);
    if (rc) return suite::invalid("create job queue", rc);
    CellSpursJobQueueHandle h = CELL_SPURS_JOBQUEUE_HANDLE_INVALID;
    if ((rc = cellSpursJobQueueOpen(&s_jq, &h))) return suite::invalid("open handle", rc);
    if ((rc = cellSpursJobQueueSemaphoreInitialize(&s_sem, &s_jq))) return suite::invalid("jq semaphore", rc);

    suite::activity("pushing %u jobs through the queue handle", kJobs);
    for (unsigned i = 0; i < kJobs; ++i) {
        std::memset(&s_job[i], 0, sizeof s_job[i]);
        std::memcpy(&s_job[i].header, SUITE_SPU_JOBHEADER, sizeof(CellSpursJobHeader));
        s_job[i].header.eaBinary += reinterpret_cast<uint64_t>(SUITE_SPU_BIN);
        s_job[i].workArea.userData[0] = reinterpret_cast<uint64_t>(&s_out[i][0]);
        s_job[i].workArea.userData[1] = kMagic + i;
        if ((rc = cellSpursJobQueuePushJob(&s_jq, h, &s_job[i].header, sizeof s_job[i], 0, &s_sem)))
            return suite::invalid("push job", rc);
    }
    if ((rc = cellSpursJobQueuePushFlush(&s_jq, h))) return suite::invalid("push flush", rc);

    int result = 0;
    suite::activity("waiting on the job-queue semaphore");
    if ((rc = cellSpursJobQueueSemaphoreAcquire(&s_sem, kJobs)))
        result = suite::fail("semaphore acquire rc", rc, 0);
    for (unsigned i = 0; i < kJobs && !result; ++i) {
        if (s_out[i][0] != kMagic + i)
            result = suite::fail("job result magic", s_out[i][0], kMagic + i);
        else if (s_out[i][3] != 0x10b6u)
            result = suite::fail("job marker", s_out[i][3], 0x10b6u);
    }
    suite::activity("closing and joining the job queue");
    cellSpursJobQueueClose(&s_jq, h);
    cellSpursShutdownJobQueue(&s_jq);
    int exitCode = 0;
    if ((rc = cellSpursJoinJobQueue(&s_jq, &exitCode)) && !result) result = suite::fail("join job queue rc", rc, 0);
    spurs->finalize();
    cellSysmoduleUnloadModule(CELL_SYSMODULE_SPURS_JQ);
    return result ? result : suite::ok();
}

SUITE_ENTRY_POINT(row_main)
