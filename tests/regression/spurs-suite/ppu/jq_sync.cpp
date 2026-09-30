/* spurs-jq-sync (row spurs-jq-sync): ordering between batches of a SPURS
 * job queue, as a pipeline uses it.  JS_ROWS ROW jobs (tag 0) each work for
 * a while and then write their row; JS_COLS COLUMN jobs read every row and
 * report how many were finished.  The queue may run on all four SPUs.
 *
 *   sync        rows, cellSpursJobQueuePortPushSync(port, 1), columns
 *   sync+flush  rows, PushSync(port, 1), PushFlush(port), columns
 *   control     rows, columns (no sync): must show a column that ran
 *               before every row finished, or the test proves nothing
 *
 * Each mode runs three times.  Prints the fewest rows any column saw:
 *   JQ_SYNC <mode> min_rows_seen=<n> of <JS_ROWS>
 * OK when both sync modes always see every row and the control does not. */
#include "harness.h"
#include <cell/spurs/job_queue.h>
#include <cell/spurs/job_queue_port.h>
#include <cell/sysmodule.h>
#include "../jq_sync.h"
#include SUITE_SPU_HEADER
#include SUITE_SPU_JOBHEADER_HEADER

#define JQ_DEPTH 32
#define JQ_POOL  (JS_ROWS + JS_COLS + 4)
#define REPS     3

enum { MODE_SYNC, MODE_SYNC_FLUSH, MODE_CONTROL };
static const char *const kModeName[] = { "sync", "sync+flush", "control" };

static CellSpursJobQueue s_jq __attribute__((aligned(128)));
static CellSpursJobQueuePort s_port __attribute__((aligned(128)));
static CellSpursJob128 s_job[JS_ROWS + JS_COLS] __attribute__((aligned(128)));
static uint64_t s_cmd[CELL_SPURS_JOBQUEUE_SIZE_COMMAND_BUFFER(JQ_DEPTH) / sizeof(uint64_t)]
    __attribute__((aligned(CELL_SPURS_JOBQUEUE_COMMAND_BUFFER_ALIGN)));
static uint8_t s_pool[CELL_SPURS_JOBQUEUE_JOB_DESCRIPTOR_POOL_SIZE(0, JQ_POOL, 0, 0, 0, 0, 0, 0)]
    __attribute__((aligned(CELL_SPURS_JOBQUEUE_JOB_DESCRIPTOR_POOL_ALIGN)));
static volatile uint32_t s_rows[JS_ROWS][4] __attribute__((aligned(128)));
static volatile uint32_t s_cols[JS_COLS][4] __attribute__((aligned(128)));
static const uint8_t s_prio[8] = { 8, 8, 8, 8, 0, 0, 0, 0 };

static void fill(CellSpursJob128 *job, uint32_t kind, uint32_t index)
{
    std::memset(job, 0, sizeof *job);
    std::memcpy(&job->header, SUITE_SPU_JOBHEADER, sizeof(CellSpursJobHeader));
    job->header.eaBinary += reinterpret_cast<uint64_t>(SUITE_SPU_BIN);
    job->workArea.userData[0] = reinterpret_cast<uint64_t>(&s_rows[0][0]);
    job->workArea.userData[1] = reinterpret_cast<uint64_t>(&s_cols[0][0]);
    job->workArea.userData[2] = kind;
    job->workArea.userData[3] = index;
}

/* one pass; returns the fewest rows a column saw, or -1 on a push or wait failure */
static int pass(int mode, int *rc)
{
    std::memset(const_cast<uint32_t *>(&s_rows[0][0]), 0, sizeof s_rows);
    std::memset(const_cast<uint32_t *>(&s_cols[0][0]), 0, sizeof s_cols);
    __sync_synchronize();
    for (unsigned i = 0; i < JS_ROWS; ++i) {
        fill(&s_job[i], JS_ROW, i);
        if ((*rc = cellSpursJobQueuePortPushJob(&s_port, &s_job[i].header, sizeof s_job[i], 0, false))) return -1;
    }
    if (mode != MODE_CONTROL && (*rc = cellSpursJobQueuePortPushSync(&s_port, 1))) return -1;
    if (mode == MODE_SYNC_FLUSH && (*rc = cellSpursJobQueuePortPushFlush(&s_port))) return -1;
    for (unsigned j = 0; j < JS_COLS; ++j) {
        fill(&s_job[JS_ROWS + j], JS_COL, j);
        if ((*rc = cellSpursJobQueuePortPushJob(&s_port, &s_job[JS_ROWS + j].header, sizeof s_job[0], 0, false))) return -1;
    }
    if ((*rc = cellSpursJobQueuePortPushFlush(&s_port))) return -1;
    if (!suite::wait_for([] {
            for (unsigned j = 0; j < JS_COLS; ++j)
                if (s_cols[j][0] != (JS_COL_DONE | j)) return false;
            for (unsigned i = 0; i < JS_ROWS; ++i)
                if (s_rows[i][0] != (JS_ROW_DONE | i)) return false;
            return true;
        })) {
        *rc = 0;
        return -1;
    }
    int least = JS_ROWS;
    for (unsigned j = 0; j < JS_COLS; ++j)
        if ((int)s_cols[j][1] < least) least = (int)s_cols[j][1];
    return least;
}

static int row_main()
{
    suite::watchdog(60);
    int rc = cellSysmoduleLoadModule(CELL_SYSMODULE_SPURS_JQ);
    if (rc) return suite::invalid("load SPURS_JQ", rc);
    auto *spurs = new cell::Spurs::Spurs2;
    if ((rc = suite::spurs_up(spurs, "SuiteJqs"))) return suite::invalid("spurs", rc);

    CellSpursJobQueueAttribute attr;
    if ((rc = cellSpursJobQueueAttributeInitialize(&attr))) return suite::invalid("jq attribute", rc);
    cellSpursJobQueueAttributeSetMaxSizeJobDescriptor(&attr, 256);
    cellSpursJobQueueAttributeSetIsHaltOnError(&attr, true);
    CellSpursJobQueueJobDescriptorPool poolDesc;
    std::memset(&poolDesc, 0, sizeof poolDesc);
    poolDesc.nJob128 = JQ_POOL;
    rc = cellSpursCreateJobQueueWithJobDescriptorPool(reinterpret_cast<CellSpurs *>(spurs), &s_jq, &attr, s_pool, &poolDesc,
                                                      "suite-jqs", s_cmd, JQ_DEPTH, 4, s_prio);
    if (rc) return suite::invalid("create job queue", rc);
    if ((rc = cellSpursJobQueuePortInitialize(&s_port, &s_jq, false))) return suite::invalid("port initialize", rc);

    int least[3];
    for (int mode = 0; mode < 3; ++mode) {
        least[mode] = JS_ROWS;
        suite::activity("mode %s", kModeName[mode]);
        for (int rep = 0; rep < REPS; ++rep) {
            int seen = pass(mode, &rc);
            if (seen < 0) return rc ? suite::invalid("push", rc) : suite::fail("jobs finished", mode, 0);
            if (seen < least[mode]) least[mode] = seen;
        }
        std::printf("JQ_SYNC %s min_rows_seen=%d of %d\n", kModeName[mode], least[mode], JS_ROWS);
        std::fflush(stdout);
    }

    cellSpursJobQueuePortFinalize(&s_port);
    cellSpursShutdownJobQueue(&s_jq);
    int exitCode = 0;
    cellSpursJoinJobQueue(&s_jq, &exitCode);
    spurs->finalize();
    cellSysmoduleUnloadModule(CELL_SYSMODULE_SPURS_JQ);

    if (least[MODE_CONTROL] == JS_ROWS)
        return suite::invalid("control never overlapped rows and columns; the sync results prove nothing", 0);
    if (least[MODE_SYNC] != JS_ROWS) return suite::fail("sync: rows finished before columns", least[MODE_SYNC], JS_ROWS);
    if (least[MODE_SYNC_FLUSH] != JS_ROWS)
        return suite::fail("sync+flush: rows finished before columns", least[MODE_SYNC_FLUSH], JS_ROWS);
    return suite::ok();
}

SUITE_ENTRY_POINT(row_main)
