/* spurs-suite SPU-side control (row spurs-control): SPU tasks drive other
 * workloads.
 *   run      a task runs a job chain the PPU created but never ran; its
 *            four jobs must all complete
 *   shutdown a task shuts that job chain down; the PPU join returns
 *   guard    a job chain waits on a job guard; a task's notify releases
 *            it and its job runs
 *   errors   argument errors of the control calls
 *   create   tasks create tasks in their own taskset, directly and through
 *            a task attribute; each created task reports its id
 *   own      a task shuts down its own taskset; the PPU join returns */
#include "harness.h"
#include "../control.h"
#include SUITE_SPU_HEADER
#include SUITE_JOB_HEADER

static const uint8_t s_prio[8] = { 8, 0, 0, 0, 0, 0, 0, 0 };
alignas(128) static volatile result_slot g_result[C_SLOTS];
alignas(128) static CellSpursJob256 s_job[C_JOBS + 1];
alignas(128) static volatile uint32_t s_out[C_JOBS + 1][4];
alignas(16) static uint64_t s_chain1[C_JOBS + 2];
alignas(16) static uint64_t s_chain2[4];
alignas(128) static CellSpursJobGuard s_guard;

static bool done(unsigned kind) { return suite::slot_done(g_result[kind], kind); }

static int run_task(cell::Spurs::Taskset *ts, unsigned kind, const void *object, const char *what)
{
    int rc = suite::launch(ts, SUITE_SPU_BIN, reinterpret_cast<uintptr_t>(g_result),
                           static_cast<uint32_t>(reinterpret_cast<uintptr_t>(object)), kind);
    if (rc)
        return suite::invalid(what, rc);
    suite::activity("waiting for task: %s", what);
    if (!suite::wait_for([&] { return done(kind); }))
        return suite::fail(what, g_result[kind].magic, RESULT_MAGIC | kind);
    if (g_result[kind].status)
        return suite::fail(what, g_result[kind].status, 0);
    return 0;
}

static void make_job(unsigned i)
{
    std::memset(&s_job[i], 0, sizeof s_job[i]);
    s_job[i].header.eaBinary = reinterpret_cast<uint64_t>(SUITE_JOB_BIN);
    s_job[i].header.sizeBinary = CELL_SPURS_GET_SIZE_BINARY(SUITE_JOB_BIN_SIZE);
    s_job[i].header.jobType = CELL_SPURS_JOB_TYPE_BINARY2;
    s_job[i].workArea.userData[0] = reinterpret_cast<uint64_t>(&s_out[i][0]);
    s_job[i].workArea.userData[1] = C_JOB_MAGIC + i;
    s_out[i][0] = 0;
}

static int make_chain(cell::Spurs::Spurs2 *spurs, CellSpursJobChain *jc, const uint64_t *commands,
                      const char *name)
{
    CellSpursJobChainAttribute attr;
    std::memset(&attr, 0, sizeof attr);
    int rc = cellSpursJobChainAttributeInitialize(&attr, commands, 256, 16, s_prio, 4, true, 0, 1, false, 256, 0);
    if (!rc) rc = cellSpursJobChainAttributeSetName(&attr, name);
    if (!rc) rc = cellSpursCreateJobChainWithAttribute(reinterpret_cast<CellSpurs *>(spurs), jc, &attr);
    return rc;
}

static int row_main()
{
    suite::watch_slots(g_result, C_SLOTS);
    suite::watchdog(40);
    std::memset((void *)g_result, 0, sizeof g_result);
    auto *spurs = new cell::Spurs::Spurs2;
    int rc = suite::spurs_up(spurs, "SuiteCtl");
    if (rc) return suite::invalid("spurs", rc);
    cell::Spurs::Taskset *ts = suite::taskset_up(spurs, &rc);
    if (!ts) return suite::invalid("taskset", rc);
    int result;

    /* run: a chain the PPU never runs */
    unsigned n = 0;
    for (unsigned i = 0; i < C_JOBS; ++i) {
        make_job(i);
        s_chain1[n++] = CELL_SPURS_JOB_COMMAND_JOB(&s_job[i]);
    }
    s_chain1[n++] = CELL_SPURS_JOB_COMMAND_LWSYNC;
    s_chain1[n++] = CELL_SPURS_JOB_COMMAND_END;
    auto *jc1 = new CellSpursJobChain;
    suite::activity("creating job chain 1 (not run from the PPU)");
    if ((rc = make_chain(spurs, jc1, s_chain1, "suite-ctl-run"))) return suite::invalid("create chain 1", rc);
    if ((result = run_task(ts, C_RUN_CHAIN, jc1, "task runs chain"))) return result;
    suite::activity("waiting for the chain's jobs");
    if (!suite::wait_for([&] {
            for (unsigned i = 0; i < C_JOBS; ++i)
                if (s_out[i][0] != C_JOB_MAGIC + i)
                    return false;
            return true;
        })) {
        unsigned ran = 0;
        for (unsigned i = 0; i < C_JOBS; ++i)
            ran += s_out[i][0] == C_JOB_MAGIC + i;
        return suite::fail("jobs run by the task's run", ran, C_JOBS);
    }

    /* shutdown: the task shuts the chain down, the PPU joins it */
    if ((result = run_task(ts, C_SHUTDOWN_CHAIN, jc1, "task shuts chain down"))) return result;
    suite::activity("joining job chain 1");
    if ((rc = cellSpursJoinJobChain(jc1))) return suite::fail("join chain 1", rc, 0);

    /* guard: a chain held by a job guard, released by a task */
    make_job(C_JOBS);
    s_chain2[0] = CELL_SPURS_JOB_COMMAND_GUARD(&s_guard);
    s_chain2[1] = CELL_SPURS_JOB_COMMAND_JOB(&s_job[C_JOBS]);
    s_chain2[2] = CELL_SPURS_JOB_COMMAND_LWSYNC;
    s_chain2[3] = CELL_SPURS_JOB_COMMAND_END;
    auto *jc2 = new CellSpursJobChain;
    suite::activity("creating job chain 2 behind a job guard");
    if ((rc = make_chain(spurs, jc2, s_chain2, "suite-ctl-guard"))) return suite::invalid("create chain 2", rc);
    if ((rc = cellSpursJobGuardInitialize(jc2, &s_guard, 1, 1, 0))) return suite::invalid("guard init", rc);
    {
        const volatile uint32_t *g = reinterpret_cast<const volatile uint32_t *>(&s_guard);
        std::printf("guard after init: %08x %08x %08x %08x %08x %08x %08x\n",
                    g[0], g[1], g[2], g[3], g[4], g[8], g[12]);
    }
    if ((rc = cellSpursRunJobChain(jc2))) return suite::invalid("run chain 2", rc);
    suite::activity("chain 2 waiting at its guard");
    sys_timer_usleep(50000);
    if (s_out[C_JOBS][0] == C_JOB_MAGIC + C_JOBS)
        return suite::fail("job ran before the guard was released", 1, 0);
    if ((result = run_task(ts, C_GUARD_NOTIFY, &s_guard, "task notifies guard"))) return result;
    suite::activity("waiting for the guarded job");
    if (!suite::wait_for([&] { return s_out[C_JOBS][0] == C_JOB_MAGIC + C_JOBS; }))
        return suite::fail("guarded job after notify", s_out[C_JOBS][0], C_JOB_MAGIC + C_JOBS);
    /* errors, while chain 2 still exists */
    if (run_task(ts, C_ERRORS, jc2, "argument errors")) {
        std::printf("errors: step %u got %#x\n", g_result[C_ERRORS].value, g_result[C_ERRORS].extra);
        return 1;
    }

    suite::activity("shutting down and joining job chain 2");
    if ((rc = cellSpursShutdownJobChain(jc2))) return suite::fail("shutdown chain 2", rc, 0);
    if ((rc = cellSpursJoinJobChain(jc2))) return suite::fail("join chain 2", rc, 0);

    /* create: tasks create tasks, directly and through a task attribute */
    alignas(16) static ctl_params params;
    params.elf = reinterpret_cast<uintptr_t>(SUITE_SPU_BIN);
    params.slots = reinterpret_cast<uintptr_t>(g_result);
    for (auto &ctx : params.context)
        ctx = reinterpret_cast<uintptr_t>(::aligned_alloc(CELL_SPURS_TASK_CONTEXT_ALIGN, CELL_SPURS_TASK_CONTEXT_SIZE_ALL));
    const struct { unsigned creator, child; const char *what; } creates[] = {
        { C_CREATE, C_CHILD, "task creates a task" },
        { C_CREATE_ATTR, C_CHILD2, "task creates a task from an attribute" },
    };
    for (const auto &c : creates) {
        if ((result = run_task(ts, c.creator, &params, c.what))) return result;
        suite::activity("waiting for the created task (id %u)", g_result[c.creator].value);
        if (!suite::wait_for([&] { return done(c.child); }))
            return suite::fail("created task never ran", g_result[c.child].magic, RESULT_MAGIC | c.child);
        if (g_result[c.child].value != g_result[c.creator].value)
            return suite::fail("created task id", g_result[c.child].value, g_result[c.creator].value);
    }

    /* own: a task shuts down its own taskset */
    suite::taskset_down(ts);
    cell::Spurs::Taskset *own = suite::taskset_up(spurs, &rc);
    if (!own) return suite::invalid("taskset 2", rc);
    if ((result = run_task(own, C_SHUTDOWN_OWN, nullptr, "task shuts own taskset down"))) return result;
    suite::activity("joining the self-shut-down taskset");
    if ((rc = own->join())) return suite::fail("join own taskset", rc, 0);

    spurs->finalize();
    return suite::ok();
}

SUITE_ENTRY_POINT(row_main)
