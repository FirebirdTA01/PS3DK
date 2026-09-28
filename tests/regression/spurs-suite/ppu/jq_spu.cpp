/* spurs-suite SPU job-queue runtime (row spurs-jq-spu).
 *   info    a job opens and closes its own handle and reads the queue's
 *           handle count, SPURS, max descriptor size, workload id, error
 *   wait    a job suspends itself with WaitSignal; another job wakes it
 *           with SendSignal and the job finishes
 *   sem     an SPU task initializes a job-queue semaphore, waits in
 *           Acquire, and two jobs pushed with the semaphore release it */
#include "harness.h"
#include <cell/spurs/job_queue.h>
#include <cell/spurs/job_queue_semaphore.h>
#include <cell/spurs/job_queue_port.h>
#include <cell/spurs/job_queue_port2.h>
#include <cell/sysmodule.h>
#include <initializer_list>
#include "../jq_spu.h"
#include SUITE_SPU_HEADER
#include SUITE_JOB_HEADER
#include SUITE_JOB_JOBHEADER_HEADER

#define JQ_DEPTH 16
#define JQ_POOL  Q_POOL
enum { J_INFO, J_WAIT, J_PLAIN0, J_PLAIN1, J_PUSH, J_CHILD0, J_CHILD1, J_PORT, J_CHILD2, J_CHILD3, J_PORT2, J_CHILD4, J_CHILD5, J_PORT2S, J_TPLAIN, J_TSLOW, J_SIGNAL, J_SUSPSIZE, J_COUNT };

static CellSpursJobQueue s_jq __attribute__((aligned(128)));
/* the task's semaphore, and its parameters in the next line */
static struct alignas(128) {
    CellSpursJobQueueSemaphore sem;
    alignas(128) jq_task_params prm;
} s_task;
#define s_sem (s_task.sem)
static CellSpursJobQueuePort2 s_port2c __attribute__((aligned(128)));
static CellSpursJobQueueSemaphore s_sem2 __attribute__((aligned(128)));
static CellSpursJobQueuePort s_port1 __attribute__((aligned(128)));
static CellSpursJobQueuePort s_port2 __attribute__((aligned(128)));
alignas(128) static uint8_t s_descBuf[128];
static CellSpursJobQueuePort2 s_port2a __attribute__((aligned(128)));
static CellSpursJobQueuePort2 s_port2b __attribute__((aligned(128)));
alignas(16) static CellSpursJobList s_list;
static CellSpursJob256 s_ss[Q_SS_CASES] __attribute__((aligned(128)));
alignas(16) static uint32_t s_ssSize[(Q_SS_CASES + 3) & ~3];
alignas(16) static volatile uint32_t s_ssOut[Q_SS_CASES][4];
alignas(128) static uint8_t s_ssBuf[1024];
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
    /* the signal comes from another job */
    make_job(J_SIGNAL, Q_SIGNAL, reinterpret_cast<uintptr_t>(susp));
    if ((rc = cellSpursJobQueuePushJob(&s_jq, h, &s_job[J_SIGNAL].header, sizeof s_job[J_SIGNAL], 0, nullptr)))
        return suite::invalid("push job", rc);
    if ((rc = cellSpursJobQueuePushFlush(&s_jq, h))) return suite::invalid("push flush", rc);
    if (!suite::wait_for([] { return s_out[J_SIGNAL][0] == Q_MAGIC + Q_SIGNAL; }))
        return suite::fail("signal job", s_out[J_SIGNAL][0], Q_MAGIC + Q_SIGNAL);
    if (s_out[J_SIGNAL][1])
        return suite::fail("signal job step (SPU SendSignal rc in the log)", s_out[J_SIGNAL][1], s_out[J_SIGNAL][2]);
    if (!suite::wait_for([] { return s_out[J_WAIT][0] == Q_MAGIC + Q_WAIT; }))
        return suite::fail("wait job resumed", s_out[J_WAIT][0], Q_MAGIC + Q_WAIT);
    if (s_out[J_WAIT][1] != 0)
        return suite::fail("WaitSignal rc", s_out[J_WAIT][1], 0);

    /* suspended-job sizes: the SPU runtime must agree with the PPU's */
    for (unsigned i = 0; i < Q_SS_CASES; ++i) {
        std::memset(&s_ss[i], 0, sizeof s_ss[i]);
        std::memcpy(&s_ss[i].header, SUITE_JOB_JOBHEADER, sizeof(CellSpursJobHeader));
        s_ss[i].header.eaBinary += reinterpret_cast<uint64_t>(SUITE_JOB_BIN);
        s_ssSize[i] = sizeof s_ss[i];
    }
    const uint64_t buf = reinterpret_cast<uintptr_t>(s_ssBuf);
    s_ss[1].header.sizeDmaList = 16;                         /* input list, two entries */
    s_ss[1].header.sizeInOrInOut = 256;
    s_ss[1].workArea.dmaList[0] = (128ull << 32) | buf;
    s_ss[1].workArea.dmaList[1] = (64ull << 32) | (buf + 128);
    s_ss[2].header.sizeCacheDmaList = 16;                    /* cache list, two entries */
    s_ss[2].workArea.dmaList[0] = (256ull << 32) | (buf + 256);
    s_ss[2].workArea.dmaList[1] = (512ull << 32) | (buf + 512);
    s_ss[3].header.jobType |= 2;                              /* memory checker guards */
    s_ss[3].header.sizeStack = 64;
    s_ss[3].header.sizeScratch = 4;
    s_ss[3].header.sizeOut = 128;
    s_ss[3].header.sizeInOrInOut = 256;
    s_ss[3].header.sizeDmaList = 8;
    s_ss[3].workArea.dmaList[0] = (32ull << 32) | buf;
    s_ssSize[4] = 128;                                        /* too small to suspend */
    s_ss[5].header.sizeDmaList = 8;                           /* misaligned list entry */
    s_ss[5].header.sizeInOrInOut = 256;
    s_ss[5].workArea.dmaList[0] = (32ull << 32) | (buf + 8);
    make_job(J_SUSPSIZE, Q_SUSPSIZE, 0);
    s_job[J_SUSPSIZE].workArea.userData[4] = reinterpret_cast<uintptr_t>(s_ss);
    s_job[J_SUSPSIZE].workArea.userData[5] = reinterpret_cast<uintptr_t>(&s_ssOut[0][0]);
    s_job[J_SUSPSIZE].workArea.userData[6] = reinterpret_cast<uintptr_t>(s_ssSize);
    suite::activity("comparing suspended-job sizes with the SPU runtime");
    if ((rc = cellSpursJobQueuePushJob(&s_jq, h, &s_job[J_SUSPSIZE].header, sizeof s_job[J_SUSPSIZE], 0, nullptr)))
        return suite::invalid("push job", rc);
    if ((rc = cellSpursJobQueuePushFlush(&s_jq, h))) return suite::invalid("push flush", rc);
    if (!suite::wait_for([] { return s_out[J_SUSPSIZE][0] == Q_MAGIC + Q_SUSPSIZE; }))
        return suite::fail("suspended-size job", s_out[J_SUSPSIZE][0], Q_MAGIC + Q_SUSPSIZE);
    for (unsigned i = 0; i < Q_SS_CASES; ++i)
        for (unsigned attr = 0; attr < 2; ++attr) {
            unsigned size = 0xdead;
            const int want = cellSpursJobQueueGetSuspendedJobSize(&s_ss[i].header, s_ssSize[i],
                                 static_cast<CellSpursJobQueueSuspendedJobAttribute>(attr), &size);
            std::printf("suspended size case %u attr %u: PPU rc %#x size %u, SPU rc %#x size %u\n",
                        i, attr, static_cast<unsigned>(want), size, s_ssOut[i][2 * attr], s_ssOut[i][2 * attr + 1]);
            if (s_ssOut[i][2 * attr] != static_cast<uint32_t>(want))
                return suite::fail("SPU GetSuspendedJobSize rc", s_ssOut[i][2 * attr], static_cast<uint32_t>(want));
            if (want == 0 && s_ssOut[i][2 * attr + 1] != size)
                return suite::fail("SPU GetSuspendedJobSize size", s_ssOut[i][2 * attr + 1], size);
        }

    /* a job pushes two jobs itself */
    if ((rc = cellSpursJobQueueSemaphoreInitialize(&s_sem2, &s_jq))) return suite::invalid("jq semaphore 2", rc);
    make_job(J_CHILD0, Q_PLAIN, 0);
    make_job(J_CHILD1, Q_PLAIN, 0);
    make_job(J_PUSH, Q_PUSH, reinterpret_cast<uintptr_t>(&s_sem2));
    s_job[J_PUSH].workArea.userData[4] = reinterpret_cast<uintptr_t>(&s_job[J_CHILD0]);
    s_job[J_PUSH].workArea.userData[5] = reinterpret_cast<uintptr_t>(&s_job[J_CHILD1]);
    suite::activity("pushing a job that pushes two jobs");
    if ((rc = cellSpursJobQueuePushJob(&s_jq, h, &s_job[J_PUSH].header, sizeof s_job[J_PUSH], 0, nullptr)))
        return suite::invalid("push job", rc);
    if ((rc = cellSpursJobQueuePushFlush(&s_jq, h))) return suite::invalid("push flush", rc);
    if (!suite::wait_for([] { return s_out[J_PUSH][0] == Q_MAGIC + Q_PUSH; }))
        return suite::fail("pushing job", s_out[J_PUSH][0], Q_MAGIC + Q_PUSH);
    if (s_out[J_PUSH][1]) {
        std::printf("push job: step %u got %#x want %#x\n", s_out[J_PUSH][1], s_out[J_PUSH][2], s_out[J_PUSH][3]);
        return suite::fail("push job step", s_out[J_PUSH][1], 0);
    }
    suite::activity("acquiring the children's semaphore");
    if ((rc = cellSpursJobQueueSemaphoreAcquire(&s_sem2, 2))) return suite::fail("children semaphore", rc, 0);
    for (unsigned i : { J_CHILD0, J_CHILD1 })
        if (s_out[i][0] != Q_MAGIC + Q_PLAIN)
            return suite::fail("child job ran", s_out[i][0], Q_MAGIC + Q_PLAIN);

    /* ports set up and pushed through by a job, synced and finalized here */
    make_job(J_CHILD2, Q_PLAIN, 0);
    make_job(J_CHILD3, Q_PLAIN, 0);
    make_job(J_PORT, Q_PORT, reinterpret_cast<uintptr_t>(&s_port1));
    s_job[J_PORT].workArea.userData[4] = reinterpret_cast<uintptr_t>(&s_job[J_CHILD2]);
    s_job[J_PORT].workArea.userData[5] = reinterpret_cast<uintptr_t>(&s_port2);
    s_job[J_PORT].workArea.userData[6] = reinterpret_cast<uintptr_t>(&s_job[J_CHILD3]);
    s_job[J_PORT].workArea.userData[7] = reinterpret_cast<uintptr_t>(s_descBuf);
    std::memset(&s_port1, 0, sizeof s_port1);
    std::memset(&s_port2, 0, sizeof s_port2);
    suite::activity("pushing a job that sets up two ports");
    if ((rc = cellSpursJobQueuePushJob(&s_jq, h, &s_job[J_PORT].header, sizeof s_job[J_PORT], 0, nullptr)))
        return suite::invalid("push job", rc);
    if ((rc = cellSpursJobQueuePushFlush(&s_jq, h))) return suite::invalid("push flush", rc);
    if (!suite::wait_for([] { return s_out[J_PORT][0] == Q_MAGIC + Q_PORT; }))
        return suite::fail("port job", s_out[J_PORT][0], Q_MAGIC + Q_PORT);
    if (s_out[J_PORT][1]) {
        std::printf("port job: step %u got %#x want %#x\n", s_out[J_PORT][1], s_out[J_PORT][2], s_out[J_PORT][3]);
        return suite::fail("port job step", s_out[J_PORT][1], 0);
    }
    suite::activity("syncing the job's ports from the PPU");
    if ((rc = cellSpursJobQueuePortSync(&s_port1))) return suite::fail("PPU sync of the SPU-made port", rc, 0);
    if ((rc = cellSpursJobQueuePortSync(&s_port2))) return suite::fail("PPU sync of the copy-push port", rc, 0);
    for (unsigned i : { J_CHILD2, J_CHILD3 })
        if (s_out[i][0] != Q_MAGIC + Q_PLAIN)
            return suite::fail("port job's child ran", s_out[i][0], Q_MAGIC + Q_PLAIN);
    if ((rc = cellSpursJobQueuePortFinalize(&s_port1))) return suite::fail("PPU finalize of port 1", rc, 0);
    if ((rc = cellSpursJobQueuePortFinalize(&s_port2))) return suite::fail("PPU finalize of port 2", rc, 0);

    /* Port2 made and pushed through by a job, synced and destroyed here */
    make_job(J_CHILD4, Q_PLAIN, 0);
    make_job(J_CHILD5, Q_PLAIN, 0);
    s_list.numJobs = 1;
    s_list.sizeOfJob = sizeof s_job[J_CHILD5];
    s_list.eaJobList = reinterpret_cast<uintptr_t>(&s_job[J_CHILD5]);
    make_job(J_PORT2, Q_PORT2, reinterpret_cast<uintptr_t>(&s_port2a));
    s_job[J_PORT2].workArea.userData[4] = reinterpret_cast<uintptr_t>(&s_job[J_CHILD4]);
    s_job[J_PORT2].workArea.userData[5] = reinterpret_cast<uintptr_t>(&s_list);
    std::memset(&s_port2a, 0xa5, sizeof s_port2a);
    suite::activity("pushing a job that creates a Port2");
    if ((rc = cellSpursJobQueuePushJob(&s_jq, h, &s_job[J_PORT2].header, sizeof s_job[J_PORT2], 0, nullptr)))
        return suite::invalid("push job", rc);
    if ((rc = cellSpursJobQueuePushFlush(&s_jq, h))) return suite::invalid("push flush", rc);
    if (!suite::wait_for([] { return s_out[J_PORT2][0] == Q_MAGIC + Q_PORT2; }))
        return suite::fail("port2 job", s_out[J_PORT2][0], Q_MAGIC + Q_PORT2);
    if (s_out[J_PORT2][1]) {
        std::printf("port2 job: step %u got %#x want %#x\n", s_out[J_PORT2][1], s_out[J_PORT2][2], s_out[J_PORT2][3]);
        return suite::fail("port2 job step", s_out[J_PORT2][1], 0);
    }
    suite::activity("syncing and destroying the job's Port2 from the PPU");
    if (cellSpursJobQueuePort2GetJobQueue(&s_port2a) != &s_jq)
        return suite::fail("PPU reads the SPU-made Port2's job queue", 0, 0);
    if ((rc = cellSpursJobQueuePort2Sync(&s_port2a, 0))) return suite::fail("PPU sync of the SPU-made Port2", rc, 0);
    for (unsigned i : { J_CHILD4, J_CHILD5 })
        if (s_out[i][0] != Q_MAGIC + Q_PLAIN)
            return suite::fail("port2 job's child ran", s_out[i][0], Q_MAGIC + Q_PLAIN);
    if ((rc = cellSpursJobQueuePort2Destroy(&s_port2a))) return suite::fail("PPU destroy of the SPU-made Port2", rc, 0);

    /* Port2 made here, synced and destroyed by a job */
    if ((rc = cellSpursJobQueuePort2Create(&s_port2b, &s_jq))) return suite::invalid("PPU Port2 create", rc);
    make_job(J_PORT2S, Q_PORT2S, reinterpret_cast<uintptr_t>(&s_port2b));
    suite::activity("pushing a job that destroys the PPU's Port2");
    if ((rc = cellSpursJobQueuePushJob(&s_jq, h, &s_job[J_PORT2S].header, sizeof s_job[J_PORT2S], 0, nullptr)))
        return suite::invalid("push job", rc);
    if ((rc = cellSpursJobQueuePushFlush(&s_jq, h))) return suite::invalid("push flush", rc);
    if (!suite::wait_for([] { return s_out[J_PORT2S][0] == Q_MAGIC + Q_PORT2S; }))
        return suite::fail("port2-sync job", s_out[J_PORT2S][0], Q_MAGIC + Q_PORT2S);
    if (s_out[J_PORT2S][1]) {
        std::printf("port2-sync job: step %u got %#x want %#x\n", s_out[J_PORT2S][1], s_out[J_PORT2S][2], s_out[J_PORT2S][3]);
        return suite::fail("port2-sync job step", s_out[J_PORT2S][1], 0);
    }
    if ((rc = cellSpursJobQueuePort2Destroy(&s_port2b)) == CELL_OK)
        return suite::fail("PPU destroy after the job destroyed the Port2", rc, 0x80410a0f);

    /* semaphore: a task waits for two jobs */
    make_job(J_TPLAIN, Q_PLAIN, 0);
    make_job(J_TSLOW, Q_SLOW, 0);
    s_task.prm.port2 = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(&s_port2c));
    s_task.prm.plain = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(&s_job[J_TPLAIN]));
    s_task.prm.slow = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(&s_job[J_TSLOW]));
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
    suite::activity("the task allocates from the descriptor pool and copy-pushes");
    if (!suite::wait_for([] { return g_result[0].value == 3 || g_result[0].status; }))
        return suite::fail("pool task done", g_result[0].value, 3);
    if (g_result[0].status) {
        std::printf("pool task: step %u got %#x\n", g_result[0].status, g_result[0].extra);
        return suite::fail("pool task step", g_result[0].status, 0);
    }
    if (s_out[J_TSLOW][0] != Q_MAGIC + Q_SLOW || s_out[J_TPLAIN][0] != Q_MAGIC + Q_PLAIN)
        return suite::fail("pool jobs ran", s_out[J_TSLOW][0], Q_MAGIC + Q_SLOW);

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
