/* spurs-suite job extras (row spurs-job-extras).  A task works on a job
 * chain that exists but has not run: it sets the grab limit, fills the
 * four urgent slots (three jobs and a call to a one-job list) and finds
 * no fifth, and fills a job header with SetJobbin2Param.  The PPU checks
 * the chain's grab limit and urgent slots and compares the header with
 * its own SetJobbin2Param of the same image, then runs the chain: its
 * two list jobs and all four urgent jobs must run, and the memory-check
 * job must see a clean check and then the null-pointer guard it broke. */
#include "harness.h"
#include "../job_extras.h"
#include SUITE_SPU_HEADER
#include SUITE_JOB_HEADER
#include SUITE_JB2_HEADER

enum { J_MAIN, J_MEMCHECK, J_URGENT0, J_COUNT = J_URGENT0 + X_URGENT_JOBS };

static const uint8_t s_prio[8] = { 8, 0, 0, 0, 0, 0, 0, 0 };
alignas(128) static volatile result_slot g_result[1];
alignas(128) static x_params s_params;
alignas(128) static CellSpursJob256 s_job[J_COUNT];
alignas(128) static volatile uint32_t s_out[J_COUNT][4];
alignas(128) static uint8_t s_header[0x30];
alignas(16) static uint64_t s_list[4];
alignas(16) static uint64_t s_call[2];

static void make_job(unsigned i, uint64_t mode)
{
    std::memset(&s_job[i], 0, sizeof s_job[i]);
    s_job[i].header.eaBinary = reinterpret_cast<uint64_t>(SUITE_JOB_BIN);
    s_job[i].header.sizeBinary = CELL_SPURS_GET_SIZE_BINARY(SUITE_JOB_BIN_SIZE);
    s_job[i].header.jobType = CELL_SPURS_JOB_TYPE_BINARY2 |
                              (mode == X_JOB_MEMCHECK ? CELL_SPURS_JOB_TYPE_MEMORY_CHECK : 0);
    s_job[i].workArea.userData[0] = reinterpret_cast<uint64_t>(&s_out[i][0]);
    s_job[i].workArea.userData[1] = X_JOB_MAGIC + i;
    s_job[i].workArea.userData[2] = mode;
    s_out[i][0] = 0;
}

static int row_main()
{
    suite::watch_slots(g_result, 1);
    suite::watchdog(40);
    std::memset((void *)g_result, 0, sizeof g_result);
    auto *spurs = new cell::Spurs::Spurs2;
    int rc = suite::spurs_up(spurs, "SuiteJx");
    if (rc) return suite::invalid("spurs", rc);
    cell::Spurs::Taskset *ts = suite::taskset_up(spurs, &rc);
    if (!ts) return suite::invalid("taskset", rc);

    make_job(J_MAIN, X_JOB_PLAIN);
    make_job(J_MEMCHECK, 2);   /* diagnostic */
    for (unsigned i = 0; i < X_URGENT_JOBS; ++i)
        make_job(J_URGENT0 + i, X_JOB_PLAIN);
    s_list[0] = CELL_SPURS_JOB_COMMAND_JOB(&s_job[J_MAIN]);
    s_list[1] = CELL_SPURS_JOB_COMMAND_JOB(&s_job[J_MEMCHECK]);
    s_list[2] = CELL_SPURS_JOB_COMMAND_LWSYNC;
    s_list[3] = CELL_SPURS_JOB_COMMAND_END;
    s_call[0] = CELL_SPURS_JOB_COMMAND_JOB(&s_job[J_URGENT0 + 1]);
    s_call[1] = CELL_SPURS_JOB_COMMAND_RET;

    suite::activity("creating a job chain (not run yet)");
    auto *jc = new CellSpursJobChain;
    CellSpursJobChainAttribute attr;
    std::memset(&attr, 0, sizeof attr);
    rc = cellSpursJobChainAttributeInitialize(&attr, s_list, 256, 16, s_prio, 4, true, 0, 1, false, 256, 0);
    if (!rc) rc = cellSpursJobChainAttributeSetName(&attr, "suite-jx");
    if (!rc) rc = cellSpursJobChainAttributeSetJobTypeMemoryCheck(&attr);
    if (!rc) rc = cellSpursCreateJobChainWithAttribute(reinterpret_cast<CellSpurs *>(spurs), jc, &attr);
    if (rc) return suite::invalid("create chain", rc);

    std::memset(&s_params, 0, sizeof s_params);
    s_params.chain = reinterpret_cast<uintptr_t>(jc);
    s_params.jobs[0] = reinterpret_cast<uintptr_t>(&s_job[J_URGENT0]);
    for (unsigned i = 2; i < X_URGENT_JOBS; ++i)
        s_params.jobs[i] = reinterpret_cast<uintptr_t>(&s_job[J_URGENT0 + i]);
    s_params.callList = reinterpret_cast<uintptr_t>(s_call);
    s_params.jobbin2 = reinterpret_cast<uintptr_t>(SUITE_JB2_BIN);
    s_params.header = reinterpret_cast<uintptr_t>(s_header);

    if ((rc = suite::launch(ts, SUITE_SPU_BIN, reinterpret_cast<uintptr_t>(g_result),
                            static_cast<uint32_t>(reinterpret_cast<uintptr_t>(&s_params)), 0)))
        return suite::invalid("launch task", rc);
    suite::activity("waiting for the task");
    if (!suite::wait_for([&] { return suite::slot_done(g_result[0], 0); }))
        return suite::fail("task report", g_result[0].magic, RESULT_MAGIC);
    if (g_result[0].status) {
        std::printf("job extras: step %u got %#x want %#x\n", g_result[0].status,
                    g_result[0].value, g_result[0].extra);
        return suite::fail("task step", g_result[0].status, 0);
    }

    /* what the task left in the chain */
    const volatile uint8_t *chain = reinterpret_cast<const volatile uint8_t *>(jc);
    const unsigned maxGrab = (chain[0x72] << 8) | chain[0x73];
    if (maxGrab != X_MAX_GRAB)
        return suite::fail("chain grab limit", maxGrab, X_MAX_GRAB);
    for (unsigned i = 0; i < X_URGENT_JOBS; ++i)
        if (!*reinterpret_cast<const volatile uint64_t *>(chain + 0x30 + i * 8))
            return suite::fail("urgent slot filled", i, 1);

    /* SetJobbin2Param from the SPU matches the PPU's */
    alignas(16) CellSpursJobHeader mine;
    std::memset(&mine, 0, sizeof mine);
    if ((rc = cellSpursJobHeaderSetJobbin2Param(&mine, SUITE_JB2_BIN)))
        return suite::invalid("PPU SetJobbin2Param", rc);
    for (unsigned i = 0; i < sizeof mine; ++i)
        if (s_header[i] != reinterpret_cast<const uint8_t *>(&mine)[i]) {
            std::printf("jobbin2 header byte %#x: spu %#x ppu %#x\n", i, s_header[i],
                        reinterpret_cast<const uint8_t *>(&mine)[i]);
            return suite::fail("SPU SetJobbin2Param header", s_header[i], reinterpret_cast<const uint8_t *>(&mine)[i]);
        }

    suite::activity("running the chain: 2 list jobs + 4 urgent");
    if ((rc = cellSpursRunJobChain(jc))) return suite::invalid("run chain", rc);
    if (!suite::wait_for([&] {
            for (unsigned i = 0; i < J_COUNT; ++i)
                if (s_out[i][0] != X_JOB_MAGIC + i)
                    return false;
            return true;
        })) {
        unsigned ran = 0;
        for (unsigned i = 0; i < J_COUNT; ++i)
            ran |= (s_out[i][0] == X_JOB_MAGIC + i) << i;
        return suite::fail("jobs that ran (bit per job)", ran, (1u << J_COUNT) - 1);
    }
    std::printf("diag: job $1 word0 %#x word1 %#x ctx %#x\n", s_out[J_MEMCHECK][1], s_out[J_MEMCHECK][2], s_out[J_MEMCHECK][3]);
    return suite::fail("diagnostic run", 0, 1);
    if (s_out[J_MEMCHECK][1] != 0)
        return suite::fail("memory check initialize", s_out[J_MEMCHECK][1], 0);
    if (s_out[J_MEMCHECK][2] != 0)
        return suite::fail("clean memory check (rc | cause << 16)", s_out[J_MEMCHECK][2], 0);
    if (s_out[J_MEMCHECK][3] != (0x0a12u | (0x80u << 16)))
        return suite::fail("broken null guard (rc | cause << 16)", s_out[J_MEMCHECK][3], 0x0a12u | (0x80u << 16));

    suite::activity("shutting down the chain");
    if ((rc = cellSpursShutdownJobChain(jc))) return suite::fail("shutdown chain", rc, 0);
    if ((rc = cellSpursJoinJobChain(jc))) return suite::fail("join chain", rc, 0);
    suite::taskset_down(ts);
    spurs->finalize();
    return suite::ok();
}

SUITE_ENTRY_POINT(row_main)
