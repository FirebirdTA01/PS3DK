/* spurs-suite task services (rows spurs-task-manual and spurs-task-driver):
 * task getters, the task stack placement, yield, a pending and a blocking
 * wait-signal, and semaphore P/V between two tasks with the consumer
 * blocking.  The two rows run the same SPU source linked by hand and with
 * the -mspurs-task driver mode. */
#include "harness.h"
#include "../task_services.h"
#include SUITE_SPU_HEADER

alignas(128) static volatile result_slot g_slot[TS_SLOTS];
alignas(128) static CellSpursSemaphore g_sem;

static int row_main()
{
    suite::watch_slots(g_slot, TS_SLOTS);
    auto *spurs = new cell::Spurs::Spurs2;
    int rc = suite::spurs_up(spurs, "SuiteTs");
    if (rc) return suite::invalid("spurs", rc);
    cell::Spurs::Taskset *ts = suite::taskset_up(spurs, &rc);
    if (!ts) return suite::invalid("taskset", rc);
    const uint64_t slots = reinterpret_cast<uint64_t>(&g_slot[0]);
    auto run = [&](unsigned kind) { return suite::launch(ts, SUITE_SPU_BIN, slots, 0, kind); };
    auto done = [&](unsigned kind) { return suite::slot_done(g_slot[kind], kind); };
    int result = 0;

    /* getters and stack */
    if ((rc = run(TS_GETTERS))) return suite::invalid("launch getters", rc);
    if (!suite::wait_for([&] { return done(TS_GETTERS); })) return suite::fail("getters never reported", 0, 1);
    if (g_slot[TS_GETTERS].extra < 0x3000)
        result = suite::fail("task stack below 0x3000", g_slot[TS_GETTERS].extra, 0x3000);
    /* cellSpursMain saves its link register at 16(crt SP): that slot must
       lie inside LS, not wrap to LS 0 in the kernel's area */
    else if (g_slot[TS_CRT_FRAME].value < 0x3000 || g_slot[TS_CRT_FRAME].value > 0x3ffe0)
        result = suite::fail("crt frame outside the task area", g_slot[TS_CRT_FRAME].value, 0x3ffe0);
    else if (g_slot[TS_GETTERS_TASKSET].value != static_cast<uint32_t>(reinterpret_cast<uintptr_t>(ts)))
        result = suite::fail("cellSpursGetTasksetAddress", g_slot[TS_GETTERS_TASKSET].value, static_cast<uint32_t>(reinterpret_cast<uintptr_t>(ts)));
    else if (g_slot[TS_GETTERS_SPURS].value != static_cast<uint32_t>(reinterpret_cast<uintptr_t>(spurs)))
        result = suite::fail("cellSpursGetSpursAddress", g_slot[TS_GETTERS_SPURS].value, static_cast<uint32_t>(reinterpret_cast<uintptr_t>(spurs)));
    else if ((g_slot[TS_GETTERS_SPURS].extra & 0xff) >= 4)
        result = suite::fail("cellSpursGetCurrentSpuId", g_slot[TS_GETTERS_SPURS].extra & 0xff, 3);
    std::printf("getters: task %u sp %#x spu %u workload %u\n", g_slot[TS_GETTERS].value, g_slot[TS_GETTERS].extra,
                g_slot[TS_GETTERS_SPURS].extra & 0xff, g_slot[TS_GETTERS_SPURS].extra >> 8);

    /* yield, pending wait-signal */
    if (!result && (rc = run(TS_YIELD))) return suite::invalid("launch yield", rc);
    if (!result && !suite::wait_for([&] { return done(TS_YIELD); })) return suite::fail("yield never returned", 0, 1);
    if (!result && g_slot[TS_YIELD].status) result = suite::fail("cellSpursYield rc", g_slot[TS_YIELD].status, 0);
    if (!result && (rc = run(TS_SIGNAL_SELF))) return suite::invalid("launch signal-self", rc);
    if (!result && !suite::wait_for([&] { return done(TS_SIGNAL_SELF); })) return suite::fail("pending wait-signal never returned", 0, 1);
    if (!result && g_slot[TS_SIGNAL_SELF].status) result = suite::fail("pending wait-signal rc", g_slot[TS_SIGNAL_SELF].status, 0);

    /* blocking wait-signal: the task is switched out and back in */
    if (!result && (rc = run(TS_WAIT_SIGNAL))) return suite::invalid("launch wait-signal", rc);
    if (!result && !suite::wait_for([&] { return done(TS_WAIT_SIGNAL_READY); })) return suite::fail("wait-signal task never started", 0, 1);
    if (!result) {
        sys_timer_usleep(20000);
        if ((rc = cellSpursSendSignal(ts, g_slot[TS_WAIT_SIGNAL_READY].value))) return suite::invalid("ppu send signal", rc);
        if (!suite::wait_for([&] { return done(TS_WAIT_SIGNAL); })) return suite::fail("blocked task never resumed", 0, 1);
        if (g_slot[TS_WAIT_SIGNAL].status) result = suite::fail("blocking wait-signal rc", g_slot[TS_WAIT_SIGNAL].status, 0);
    }

    /* semaphore P/V: the consumer starts first and blocks on P */
    if (!result) {
        if ((rc = cellSpursSemaphoreInitialize(ts, &g_sem, 0))) return suite::invalid("semaphore init", rc);
        const uint32_t sem = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(&g_sem));
        if ((rc = suite::launch(ts, SUITE_SPU_BIN, slots, sem, TS_SEM_CONSUMER))) return suite::invalid("launch consumer", rc);
        sys_timer_usleep(20000);
        if ((rc = suite::launch(ts, SUITE_SPU_BIN, slots, sem, TS_SEM_PRODUCER))) return suite::invalid("launch producer", rc);
        if (!suite::wait_for([&] { return done(TS_SEM_CONSUMER) && done(TS_SEM_PRODUCER); }))
            return suite::fail("semaphore tasks never finished", g_slot[TS_SEM_CONSUMER].magic, RESULT_MAGIC | TS_SEM_CONSUMER);
        if (g_slot[TS_SEM_PRODUCER].status || g_slot[TS_SEM_PRODUCER].value != TS_SEM_ROUNDS)
            result = suite::fail("semaphore V", g_slot[TS_SEM_PRODUCER].status, 0);
        else if (g_slot[TS_SEM_CONSUMER].status || g_slot[TS_SEM_CONSUMER].value != TS_SEM_ROUNDS)
            result = suite::fail("semaphore P", g_slot[TS_SEM_CONSUMER].status, 0);
    }

    suite::taskset_down(ts);
    spurs->finalize();
    return result ? result : suite::ok();
}

SUITE_ENTRY_POINT(row_main)
