/* spurs-suite SPU job-queue runtime (row spurs-jq-spu).
 *   info    a job opens and closes its own handle and reads the queue's
 *           handle count, SPURS, max descriptor size, workload id, error
 *   wait    a job suspends itself with WaitSignal; the PPU wakes it with
 *           SendSignal and the job finishes
 *   sem     an SPU task initializes a job-queue semaphore, waits in
 *           Acquire, and two jobs pushed with the semaphore release it */
#include "harness.h"
#include <cell/spurs/job_queue.h>
#include <cell/spurs/job_queue_semaphore.h>
#include <cell/sysmodule.h>
#include "../jq_spu.h"
#include SUITE_SPU_HEADER
#include SUITE_JOB_HEADER
#include SUITE_JOB_JOBHEADER_HEADER

#define JQ_DEPTH 16
#define JQ_POOL  8
enum { J_INFO, J_WAIT, J_PLAIN0, J_PLAIN1, J_COUNT };

static CellSpursJobQueue s_jq __attribute__((aligned(128)));
static CellSpursJobQueueSemaphore s_sem __attribute__((aligned(128)));
static CellSpursJob128 s_job[J_COUNT] __attribute__((aligned(128)));
static uint64_t s_cmd[CELL_SPURS_JOBQUEUE_SIZE_COMMAND_BUFFER(JQ_DEPTH) / sizeof(uint64_t)]
    __attribute__((aligned(CELL_SPURS_JOBQUEUE_COMMAND_BUFFER_ALIGN)));
static volatile uint32_t s_out[J_COUNT][4] __attribute__((aligned(128)));
alignas(128) static volatile result_slot g_result[1];
static const uint8_t s_prio[8] = { 8, 0, 0, 0, 0, 0, 0, 0 };
static uint8_t s_pool[CELL_SPURS_JOBQUEUE_JOB_DESCRIPTOR_POOL_SIZE(0, JQ_POOL, 0, 0, 0, 0, 0, 0)]
    __attribute__((aligned(CELL_SPURS_JOBQUEUE_JOB_DESCRIPTOR_POOL_ALIGN)));

static void make_job(unsigned i, unsigned mode, uint64_t arg3)
{
    std::memset(&s_job[i], 0, sizeof s_job[i]);
    std::memcpy(&s_job[i].header, SUITE_JOB_JOBHEADER, sizeof(CellSpursJobHeader));
    s_job[i].header.eaBinary += reinterpret_cast<uint64_t>(SUITE_JOB_BIN);
    s_job[i].workArea.userData[0] = reinterpret_cast<uint64_t>(&s_out[i][0]);
    s_job[i].workArea.userData[1] = mode;
    s_job[i].workArea.userData[2] = reinterpret_cast<uintptr_t>(&s_jq);
    s_job[i].workArea.userData[3] = arg3;
    s_out[i][0] = s_out[i][1] = 0;
}

static int row_main()
{
    suite::watch_slots(g_result, 1);
    suite::watchdog(40);
    std::memset((void *)g_result, 0, sizeof g_result);
    int rc = cellSysmoduleLoadModule(CELL_SYSMODULE_SPURS_JQ);
    if (rc) return suite::invalid("load SPURS_JQ", rc);
    auto *spurs = new cell::Spurs::Spurs2;
    if ((rc = suite::spurs_up(spurs, "SuiteJqs"))) return suite::invalid("spurs", rc);

    CellSpursJobQueueAttribute attr;
    if ((rc = cellSpursJobQueueAttributeInitialize(&attr))) return suite::invalid("jq attribute", rc);
    cellSpursJobQueueAttributeSetMaxSizeJobDescriptor(&attr, Q_MAX_DESC);
    CellSpursJobQueueJobDescriptorPool poolDesc;
    std::memset(&poolDesc, 0, sizeof poolDesc);
    poolDesc.nJob128 = JQ_POOL;
    rc = cellSpursCreateJobQueueWithJobDescriptorPool(reinterpret_cast<CellSpurs *>(spurs), &s_jq, &attr, s_pool,
                                                      &poolDesc, "suite-jqs", s_cmd, JQ_DEPTH, 2, s_prio);
    if (rc) return suite::invalid("create job queue", rc);
    CellSpursJobQueueHandle h = CELL_SPURS_JOBQUEUE_HANDLE_INVALID;
    if ((rc = cellSpursJobQueueOpen(&s_jq, &h))) return suite::invalid("open handle", rc);
    auto *susp = static_cast<CellSpursJobQueueSuspendedJob *>(::aligned_alloc(128, sizeof(CellSpursJobQueueSuspendedJob)));
    if (!susp) return suite::invalid("suspend buffer", -1);
    std::memset(susp, 0, 128);

    /* info and wait jobs */
    make_job(J_INFO, Q_INFO, reinterpret_cast<uintptr_t>(spurs));
    make_job(J_WAIT, Q_WAIT, reinterpret_cast<uintptr_t>(susp));
    suite::activity("pushing the info and wait jobs");
    for (unsigned i : { J_INFO, J_WAIT })
        if ((rc = cellSpursJobQueuePushJob(&s_jq, h, &s_job[i].header, sizeof s_job[i], 0, nullptr)))
            return suite::invalid("push job", rc);
    if ((rc = cellSpursJobQueuePushFlush(&s_jq, h))) return suite::invalid("push flush", rc);
    if (!suite::wait_for([] { return s_out[J_INFO][0] == Q_MAGIC + Q_INFO; }))
        return suite::fail("info job", s_out[J_INFO][0], Q_MAGIC + Q_INFO);
    if (s_out[J_INFO][1]) {
        std::printf("info job: step %u got %#x want %#x\n", s_out[J_INFO][1], s_out[J_INFO][2], s_out[J_INFO][3]);
        return suite::fail("info job step", s_out[J_INFO][1], 0);
    }

    suite::activity("waiting for the job to suspend");
    if (!suite::wait_for([] { return s_out[J_WAIT][1] == 1 || s_out[J_WAIT][0] != 0; }))
        return suite::fail("wait job reached WaitSignal", s_out[J_WAIT][1], 1);
    if (s_out[J_WAIT][0] != 0)
        return suite::fail("wait job did not suspend (WaitSignal rc)", s_out[J_WAIT][1], 0);
    sys_timer_usleep(20000);
    if ((rc = cellSpursJobQueueSendSignal(reinterpret_cast<CellSpursJobQueueWaitingJob *>(susp))))
        return suite::fail("send signal", rc, 0);
    if (!suite::wait_for([] { return s_out[J_WAIT][0] == Q_MAGIC + Q_WAIT; }))
        return suite::fail("wait job resumed", s_out[J_WAIT][0], Q_MAGIC + Q_WAIT);
    if (s_out[J_WAIT][1] != 0)
        return suite::fail("WaitSignal rc", s_out[J_WAIT][1], 0);

    /* semaphore: a task waits for two jobs */
    cell::Spurs::Taskset *ts = suite::taskset_up(spurs, &rc);
    if (!ts) return suite::invalid("taskset", rc);
    if ((rc = suite::launch(ts, SUITE_SPU_BIN, reinterpret_cast<uintptr_t>(g_result),
                            static_cast<uint32_t>(reinterpret_cast<uintptr_t>(&s_sem)),
                            static_cast<uint32_t>(reinterpret_cast<uintptr_t>(&s_jq)))))
        return suite::invalid("launch task", rc);
    suite::activity("waiting for the task to wait on the semaphore");
    const volatile uint32_t *semWord = reinterpret_cast<const volatile uint32_t *>(&s_sem);
    if (!suite::wait_for([&] { return (semWord[0] & 0x80000000u) || g_result[0].status; }))
        return suite::fail("task waiting in Acquire (semaphore word)", semWord[0], 0x80000000u);
    if (g_result[0].status) {
        std::printf("semaphore task: step %u got %#x\n", g_result[0].status, g_result[0].extra);
        return suite::fail("semaphore task step", g_result[0].status, 0);
    }
    make_job(J_PLAIN0, Q_PLAIN, 0);
    make_job(J_PLAIN1, Q_PLAIN, 0);
    suite::activity("pushing two jobs that release the semaphore");
    for (unsigned i : { J_PLAIN0, J_PLAIN1 })
        if ((rc = cellSpursJobQueuePushJob(&s_jq, h, &s_job[i].header, sizeof s_job[i], 0, &s_sem)))
            return suite::invalid("push job", rc);
    if ((rc = cellSpursJobQueuePushFlush(&s_jq, h))) return suite::invalid("push flush", rc);
    if (!suite::wait_for([] { return g_result[0].value == 2; }))
        return suite::fail("task woke from Acquire", g_result[0].value, 2);
    if (g_result[0].status)
        return suite::fail("semaphore task step", g_result[0].status, 0);

    suite::activity("closing and joining the job queue");
    suite::taskset_down(ts);
    cellSpursJobQueueClose(&s_jq, h);
    cellSpursShutdownJobQueue(&s_jq);
    int exitCode = 0;
    if ((rc = cellSpursJoinJobQueue(&s_jq, &exitCode))) return suite::fail("join job queue", rc, 0);
    spurs->finalize();
    cellSysmoduleUnloadModule(CELL_SYSMODULE_SPURS_JQ);
    return suite::ok();
}

SUITE_ENTRY_POINT(row_main)
