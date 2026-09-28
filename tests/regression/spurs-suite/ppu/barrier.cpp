/* spurs-suite barrier (row spurs-barrier): B_TASKS tasks share a barrier
 * the PPU initialized.  Each try-waits first (the barrier is not released
 * yet), notifies, then waits; one task notifies late, so the others block
 * in wait until it arrives.  Every task must pass the barrier only after
 * all B_TASKS notified.  A further task checks the taskset address and an
 * argument error through cell::Spurs::BarrierStub. */
#include "harness.h"
#include "../barrier.h"
#include SUITE_SPU_HEADER

struct barrier_block {
    CellSpursBarrier barrier;
    uint32_t counter[32];            /* the next line: shared notify counter */
};
alignas(128) static barrier_block s_block;
alignas(128) static volatile result_slot g_result[B_SLOTS];

static int row_main()
{
    suite::watch_slots(g_result, B_SLOTS);
    suite::watchdog(30);
    std::memset(&s_block, 0, sizeof s_block);
    std::memset((void *)g_result, 0, sizeof g_result);
    auto *spurs = new cell::Spurs::Spurs2;
    int rc = suite::spurs_up(spurs, "SuiteBar");
    if (rc) return suite::invalid("spurs", rc);
    cell::Spurs::Taskset *ts = suite::taskset_up(spurs, &rc);
    if (!ts) return suite::invalid("taskset", rc);

    suite::activity("initializing a barrier for %u tasks", B_TASKS);
    if ((rc = cellSpursBarrierInitialize(reinterpret_cast<CellSpursTaskset *>(ts), &s_block.barrier, B_TASKS)))
        return suite::invalid("barrier initialize", rc);

    const uint64_t slots = reinterpret_cast<uintptr_t>(g_result);
    const uint32_t ea = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(&s_block));
    for (unsigned i = 0; i < B_TASKS; ++i)
        if ((rc = suite::launch(ts, SUITE_SPU_BIN, slots, ea, i)))
            return suite::invalid("launch task", rc);

    suite::activity("waiting for all tasks to pass the barrier");
    if (!suite::wait_for([&] {
            for (unsigned i = 0; i < B_TASKS; ++i)
                if (!suite::slot_done(g_result[i], i))
                    return false;
            return true;
        })) {
        unsigned passed = 0;
        for (unsigned i = 0; i < B_TASKS; ++i)
            passed += suite::slot_done(g_result[i], i);
        return suite::fail("tasks past the barrier", passed, B_TASKS);
    }
    for (unsigned i = 0; i < B_TASKS; ++i) {
        if (g_result[i].status)
            return suite::fail("task notify/wait rc", g_result[i].status, 0);
        if (g_result[i].value != B_TASKS)
            return suite::fail("task passed before every task notified", g_result[i].value, B_TASKS);
        if (g_result[i].extra != 0x8041090Au)
            return suite::fail("try-wait before release", g_result[i].extra, 0x8041090Au);
    }

    if ((rc = suite::launch(ts, SUITE_SPU_BIN, slots, ea, B_ERRORS))) return suite::invalid("launch errors", rc);
    suite::activity("waiting for the error-path task");
    if (!suite::wait_for([&] { return suite::slot_done(g_result[B_ERRORS], B_ERRORS); }))
        return suite::fail("error task", g_result[B_ERRORS].magic, RESULT_MAGIC | B_ERRORS);
    if (g_result[B_ERRORS].status)
        return suite::fail("barrier errors", g_result[B_ERRORS].status, g_result[B_ERRORS].value);

    suite::taskset_down(ts);
    spurs->finalize();
    return suite::ok();
}

SUITE_ENTRY_POINT(row_main)
