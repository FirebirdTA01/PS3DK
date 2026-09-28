/* spurs-suite job chain (row spurs-job-chain): four BINARY2 jobs built with
 * the -mspurs-job driver mode run from one job chain; each writes its own
 * result slot.  Checks every job ran with the right arguments and a valid
 * DMA tag, the chain did not halt, and shutdown + join complete. */
#include "harness.h"
#include SUITE_SPU_HEADER

static const unsigned kJobs = 4;
static const uint32_t kMagic = 0xc0ffe000u;

static CellSpursJob256 s_job[kJobs] __attribute__((aligned(128)));
static volatile uint32_t s_out[kJobs][4] __attribute__((aligned(128)));
static uint64_t s_chain[kJobs * 2 + 2] __attribute__((aligned(16)));
static const uint8_t s_prio[8] = { 8, 0, 0, 0, 0, 0, 0, 0 };

static int row_main()
{
    auto *spurs = new cell::Spurs::Spurs2;
    int rc = suite::spurs_up(spurs, "SuiteJc");
    if (rc) return suite::invalid("spurs", rc);

    unsigned n = 0;
    for (unsigned i = 0; i < kJobs; ++i) {
        std::memset(&s_job[i], 0, sizeof s_job[i]);
        s_job[i].header.eaBinary = reinterpret_cast<uint64_t>(SUITE_SPU_BIN);
        s_job[i].header.sizeBinary = CELL_SPURS_GET_SIZE_BINARY(SUITE_SPU_BIN_SIZE);
        s_job[i].header.jobType = CELL_SPURS_JOB_TYPE_BINARY2;
        s_job[i].workArea.userData[0] = reinterpret_cast<uint64_t>(&s_out[i][0]);
        s_job[i].workArea.userData[1] = kMagic + i;
        s_chain[n++] = CELL_SPURS_JOB_COMMAND_JOB(&s_job[i]);
    }
    s_chain[n++] = CELL_SPURS_JOB_COMMAND_LWSYNC;
    s_chain[n++] = CELL_SPURS_JOB_COMMAND_END;

    CellSpursJobChainAttribute attr;
    std::memset(&attr, 0, sizeof attr);
    rc = cellSpursJobChainAttributeInitialize(&attr, s_chain, 256, 16, s_prio, 4, true, 0, 1, false, 256, 0);
    if (rc) return suite::invalid("job chain attribute", rc);
    cellSpursJobChainAttributeSetName(&attr, "suite-jc");
    cellSpursJobChainAttributeSetHaltOnError(&attr);
    auto *jc = new CellSpursJobChain;
    if ((rc = cellSpursCreateJobChainWithAttribute(reinterpret_cast<CellSpurs *>(spurs), jc, &attr))) return suite::invalid("create job chain", rc);
    suite::activity("running job chain: %u BINARY2 jobs", kJobs);
    if ((rc = cellSpursRunJobChain(jc))) return suite::invalid("run job chain", rc);

    int result = 0;
    suite::activity("waiting for job results");
    bool all = suite::wait_for([&] {
        for (unsigned i = 0; i < kJobs; ++i)
            if (s_out[i][0] != kMagic + i)
                return false;
        return true;
    });
    CellSpursJobChainInfo info;
    std::memset(&info, 0, sizeof info);
    cellSpursGetJobChainInfo(jc, &info);
    if (info.isHalted)
        result = suite::fail("job chain halted", info.statusCode, 0);
    else if (!all) {
        unsigned done = 0;
        for (unsigned i = 0; i < kJobs; ++i)
            done += s_out[i][0] == kMagic + i;
        result = suite::fail("jobs completed", done, kJobs);
    } else {
        for (unsigned i = 0; i < kJobs && !result; ++i) {
            if (s_out[i][3] != 0x10b5u)
                result = suite::fail("job marker", s_out[i][3], 0x10b5u);
            else if (s_out[i][1] > 31)
                result = suite::fail("job dma tag", s_out[i][1], 31);
            else if (s_out[i][2] != static_cast<uint32_t>(reinterpret_cast<uintptr_t>(SUITE_SPU_BIN)))
                result = suite::fail("job eaBinary", s_out[i][2], static_cast<uint32_t>(reinterpret_cast<uintptr_t>(SUITE_SPU_BIN)));
        }
    }
    std::printf("job chain: halted=%d status=%#x\n", (int)info.isHalted, (unsigned)info.statusCode);

    suite::activity("shutting down and joining job chain");
    if ((rc = cellSpursShutdownJobChain(jc)) && !result) result = suite::fail("shutdown job chain rc", rc, 0);
    if ((rc = cellSpursJoinJobChain(jc)) && !result) result = suite::fail("join job chain rc", rc, 0);
    spurs->finalize();
    return result ? result : suite::ok();
}

SUITE_ENTRY_POINT(row_main)
